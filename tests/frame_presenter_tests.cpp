#include "common/config.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/frame_presenter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

constexpr u32 WIDTH = 256;
constexpr u32 HEIGHT = 144;

void clear(metal::Renderer& renderer, MTL::Texture* texture,
           MTL::ClearColor color) {
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
    NS_STACK_SCOPED auto pass = MTL::RenderPassDescriptor::alloc()->init();
    auto* attachment = pass->colorAttachments()->object(0);
    attachment->setTexture(texture);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setClearColor(color);
    attachment->setStoreAction(MTL::StoreActionStore);
    metal_commands->getRenderCommandEncoder(pass);
    metal_commands->endEncoding();
}

void present(metal::Renderer& renderer, MTL::Texture* drawable,
             bool new_frame) {
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    renderer.getFramePresenter().present(
        static_cast<metal::CommandBuffer*>(commands.get()), drawable,
        new_frame);
}

std::array<u8, 4> centerPixel(MTL::Texture* texture) {
    std::array<u8, 4> pixel{};
    texture->getBytes(pixel.data(), WIDTH * 4,
                      MTL::Region::Make2D(WIDTH / 2, HEIGHT / 2, 1, 1), 0);
    return pixel;
}

// A refresh without a new guest frame shows what the presenter holds. The
// compositor skips drawing (and upscaling) the layers then, so whatever the
// canvas holds must not reach the drawable.
void testRefreshWithoutNewFrame(metal::Renderer& renderer,
                                MTL::Texture* drawable, FrameGeneration mode) {
    CONFIG_INSTANCE.getFrameGeneration() = mode;
    auto& presenter = renderer.getFramePresenter();
    presenter.reset();
    const std::array<MTL::ClearColor, 2> frames{
        MTL::ClearColor::Make(1.0, 0.0, 0.0, 1.0),
        MTL::ClearColor::Make(0.0, 1.0, 0.0, 1.0)};
    for (const auto& color : frames) {
        presenter.setInputScale(0.5f, 0.5f);
        auto* canvas = presenter.getCanvas(WIDTH, HEIGHT,
                                           MTL::PixelFormatBGRA8Unorm);
        check(canvas != nullptr, "Presenter gives a canvas");
        clear(renderer, canvas, color);
        present(renderer, drawable, true);
    }
    check(presenter.holdsFrames(), "Presenter holds two guest frames");

    // Not drawn by the compositor: stale or garbage content
    auto* canvas =
        presenter.getCanvas(WIDTH, HEIGHT, MTL::PixelFormatBGRA8Unorm);
    check(canvas != nullptr, "Presenter keeps the canvas");
    clear(renderer, canvas, MTL::ClearColor::Make(1.0, 0.0, 1.0, 1.0));
    present(renderer, drawable, false);
    const auto pixel = centerPixel(drawable);
    // BGRA: the shown frame is red, green or a blend of them, never magenta
    check(pixel[0] < 32, "Refresh without a new frame does not read the canvas");
    check(static_cast<u32>(pixel[1]) + pixel[2] > 200,
          "Refresh without a new frame shows a held frame");
}
// DEBUG: GPU time of the presenter per refresh at a given size
// (HYDRA_PRESENTER_BENCH=WxH, from 1280x720)
void benchmark(metal::Renderer& renderer, FrameGeneration mode, u32 width,
               u32 height) {
    CONFIG_INSTANCE.getFrameGeneration() = mode;
    auto& presenter = renderer.getFramePresenter();
    presenter.reset();
    NS_STACK_SCOPED auto descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    descriptor->setWidth(width);
    descriptor->setHeight(height);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto drawable = renderer.getDevice()->newTexture(descriptor);
    const f32 scale_x = 1280.0f / static_cast<f32>(width);
    const f32 scale_y = 720.0f / static_cast<f32>(height);
    f64 new_ms = 0.0, refresh_ms = 0.0;
    constexpr u32 FRAMES = 30;
    for (u32 i = 0; i < FRAMES + 2; i++) {
        presenter.setInputScale(scale_x, scale_y);
        auto* canvas = presenter.getCanvas(width, height,
                                           MTL::PixelFormatBGRA8Unorm);
        check(canvas != nullptr, "Benchmark canvas");
        clear(renderer, canvas,
              MTL::ClearColor::Make((i % 7) / 7.0, (i % 3) / 3.0, 0.5, 1.0));
        for (u32 refresh = 0; refresh < 2; refresh++) {
            std::unique_ptr<ICommandBuffer> commands(
                renderer.createCommandBuffer());
            auto* metal_commands =
                static_cast<metal::CommandBuffer*>(commands.get());
            presenter.present(metal_commands, drawable, refresh == 0);
            auto* native = metal_commands->getCommandBuffer()->retain();
            commands.reset();
            native->waitUntilCompleted();
            const f64 ms = (native->GPUEndTime() - native->GPUStartTime()) * 1e3;
            native->release();
            if (i >= 2)
                (refresh == 0 ? new_ms : refresh_ms) += ms;
        }
    }
    std::cout << "BENCH " << static_cast<u32>(mode) << " " << width << "x"
              << height << ": new frame " << new_ms / FRAMES
              << " ms, refresh " << refresh_ms / FRAMES << " ms GPU\n";
}
} // namespace

int main() {
    try {
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        CONFIG_INSTANCE.getFrameGeneration() = FrameGeneration::Interpolate;
        CONFIG_INSTANCE.getSharpening() = 0;
        metal::Renderer renderer;
        auto& presenter = renderer.getFramePresenter();
        if (const char* bench = std::getenv("HYDRA_PRESENTER_BENCH")) {
            u32 width = 0, height = 0;
            check(std::sscanf(bench, "%ux%u", &width, &height) == 2,
                  "HYDRA_PRESENTER_BENCH=WxH");
            for (auto mode : {FrameGeneration::Off, FrameGeneration::Interpolate,
                              FrameGeneration::MetalFx})
                benchmark(renderer, mode, width, height);
            return 0;
        }

        // Motion and depth have the size the game rendered at: the input
        // scale of the layers drawn applies from the next canvas
        check(presenter.getCanvas(WIDTH, HEIGHT, MTL::PixelFormatBGRA8Unorm),
              "Presenter gives a canvas");
        check(presenter.getInputWidth() == WIDTH &&
                  presenter.getInputHeight() == HEIGHT,
              "Without a layer the input has the canvas size");
        presenter.setInputScale(0.5f, 0.5f);
        presenter.setInputScale(0.25f, 0.25f);
        check(presenter.getCanvas(WIDTH, HEIGHT, MTL::PixelFormatBGRA8Unorm),
              "Presenter gives a canvas");
        check(presenter.getInputWidth() == WIDTH / 2 &&
                  presenter.getInputHeight() == HEIGHT / 2,
              "Input follows the largest layer scale");
        check(presenter.getCanvas(WIDTH, HEIGHT, MTL::PixelFormatBGRA8Unorm),
              "Presenter gives a canvas");
        check(presenter.getInputWidth() == WIDTH / 2,
              "A refresh that draws no layer keeps the input size");
        presenter.setInputScale(2.0f, 2.0f);
        check(presenter.getCanvas(WIDTH, HEIGHT, MTL::PixelFormatBGRA8Unorm),
              "Presenter gives a canvas");
        check(presenter.getInputWidth() == WIDTH,
              "Downscaled layers search motion at the canvas size");

        NS_STACK_SCOPED auto descriptor =
            MTL::TextureDescriptor::alloc()->init();
        descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        descriptor->setWidth(WIDTH);
        descriptor->setHeight(HEIGHT);
        descriptor->setStorageMode(MTL::StorageModeShared);
        descriptor->setUsage(MTL::TextureUsageRenderTarget |
                             MTL::TextureUsageShaderRead);
        NS_STACK_SCOPED auto drawable =
            renderer.getDevice()->newTexture(descriptor);
        testRefreshWithoutNewFrame(renderer, drawable,
                                   FrameGeneration::Interpolate);
        testRefreshWithoutNewFrame(renderer, drawable,
                                   FrameGeneration::MetalFx);

        CONFIG_INSTANCE.getFrameGeneration() = FrameGeneration::Off;
        check(!presenter.holdsFrames(),
              "Without frame generation the canvas is always drawn");
        std::cout << "Frame presenter tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

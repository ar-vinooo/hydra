#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

int main() {
    try {
        // Exercise the real cache and Metal texture views, including
        // overlapping guest memory and contained mip levels from the CTR crash
        // path.
        setenv("HYDRA_GPU_SYNC", "1", 1);
        setenv("HYDRA_DISABLE_SHADER_CACHE", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        std::vector<u8> guest_memory(65536, 0);
        metal::Renderer renderer;
        std::unique_ptr<ICommandBuffer> commands(
            renderer.createCommandBuffer());
        auto& cache = renderer.getTextureCache();
        const auto ptr = reinterpret_cast<uptr>(guest_memory.data());
        const auto descriptor = [ptr](TextureFormat format) {
            return TextureDescriptor(ptr, TextureType::_2D, format, false, 0,
                                     64, 64, 1, 4, 1, 0, 0, 0);
        };

        auto bc5 = descriptor(TextureFormat::BC5_RGUnorm);
        auto* bc5_view = cache.find(commands.get(), bc5, TextureUsage::Read);
        for (auto format :
             {TextureFormat::BC3_RGBA, TextureFormat::BC2_RGBA,
              TextureFormat::BC7_RGBAUnorm, TextureFormat::BC5_RGSnorm,
              TextureFormat::BC3_RGBA_sRGB}) {
            auto request = descriptor(format);
            auto* view =
                cache.find(commands.get(), request, TextureUsage::Read);
            check(view->getBase() != bc5_view->getBase(),
                  "Incompatible compressed format must have its own storage");
            check(view->getBase()->getDescriptor().format == format,
                  "Compressed texture uses the requested encoding");
            check(cache.find(commands.get(), request, TextureUsage::Read) ==
                      view,
                  "Repeated lookup still reuses the cached view");
        }

        auto mip = TextureDescriptor(
            ptr + bc5.getLevelOffset(1), TextureType::_2D,
            TextureFormat::BC5_RGUnorm, false, 0, 32, 32, 1, 2, 1, 0, 0, 0);
        auto* mip_view = cache.find(commands.get(), mip, TextureUsage::Read);
        check(mip_view->getBase() == bc5_view->getBase(),
              "Compatible contained mip levels reuse storage");
        mip.format = TextureFormat::BC3_RGBA;
        auto* bc3_mip = cache.find(commands.get(), mip, TextureUsage::Read);
        check(bc3_mip->getBase()->getDescriptor().format ==
                  TextureFormat::BC3_RGBA,
              "Contained mip lookup does not cross compressed encodings");
        commands.reset();

        metal::Renderer upload_renderer;
        // Two uploads encoded before submission must never share CPU-writable
        // memory. Verify actual GPU copies retain both original byte patterns.
        auto uploads = std::unique_ptr<ICommandBuffer>(
            upload_renderer.createCommandBuffer());
        auto* first = upload_renderer.allocateTextureUploadBuffer(4096);
        auto* first_native = static_cast<metal::Buffer*>(first)->getBuffer();
        std::memset(reinterpret_cast<void*>(first->getPtr()), 0x35, 4096);
        auto* readback = upload_renderer.getDevice()->newBuffer(
            8192, MTL::ResourceStorageModeShared);
        auto* encoder = static_cast<metal::CommandBuffer*>(uploads.get())
                            ->getBlitCommandEncoder();
        encoder->copyFromBuffer(first_native, 0, readback, 0, 4096);
        upload_renderer.retireTextureUploadBuffer(uploads.get(), first);
        auto* second = upload_renderer.allocateTextureUploadBuffer(4096);
        auto* second_native = static_cast<metal::Buffer*>(second)->getBuffer();
        check(first_native != second_native,
              "In-flight upload cannot be reused");
        std::memset(reinterpret_cast<void*>(second->getPtr()), 0xa7, 4096);
        encoder->copyFromBuffer(second_native, 0, readback, 4096, 4096);
        upload_renderer.retireTextureUploadBuffer(uploads.get(), second);
        uploads
            .reset(); // HYDRA_GPU_SYNC waits for GPU and completion handlers.
        auto* bytes = static_cast<const u8*>(readback->contents());
        bool intact = true;
        for (usize i = 0; i < 4096; ++i)
            intact &= bytes[i] == 0x35 && bytes[i + 4096] == 0xa7;
        readback->release();
        check(intact, "GPU uploads preserve both patterns until completion");
        auto* reused = upload_renderer.allocateTextureUploadBuffer(4096);
        auto* reused_native = static_cast<metal::Buffer*>(reused)->getBuffer();
        if (std::getenv("HYDRA_DISABLE_UPLOAD_POOL") == nullptr)
            check(reused_native == first_native ||
                      reused_native == second_native,
                  "Completed upload becomes available for reuse");
        uploads.reset(upload_renderer.createCommandBuffer());
        upload_renderer.retireTextureUploadBuffer(uploads.get(), reused);
        uploads.reset();

        // Check the ready-memory budget and size classes independently.
        auto pool_state = std::make_shared<metal::UploadBufferPool>(
            upload_renderer.getDevice(), 8192, 1);
        auto* small = pool_state->acquire(4097);
        auto* another = pool_state->acquire(4097);
        check(small->length() == 8192, "Round upload capacity to a size class");
        pool_state->recycle(small);
        pool_state->recycle(another);
        check(pool_state->getCachedBytes() == 8192,
              "Ready pool respects its budget");
        auto* recovered = pool_state->acquire(4097);
        check(recovered == small && pool_state->getCachedBytes() == 0,
              "Acquiring a ready buffer removes it from the budget");
        pool_state->recycle(recovered);

        // A completion handler must keep its pool alive after the owner exits.
        std::weak_ptr<metal::UploadBufferPool> weak_pool = pool_state;
        auto* pending = pool_state->acquire(4097);
        uploads.reset(upload_renderer.createCommandBuffer());
        static_cast<metal::CommandBuffer*>(uploads.get())
            ->retireUploadBuffer(pool_state, pending);
        pool_state.reset();
        check(!weak_pool.expired(), "Pending uploads retain their pool owner");
        uploads.reset();
        std::cout << "PASS: compressed format isolation, overlapping memory, "
                     "mip views, upload GPU lifetime, reuse and bounded pool "
                     "on Metal\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

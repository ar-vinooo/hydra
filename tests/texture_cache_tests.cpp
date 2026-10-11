#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

#include <cmath>
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
        setenv("HYDRA_PERF_STATS", "1", 1);
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

        // A bridge merges initially separate intervals after a GPU write.
        // Disjoint sampled textures must remain untouched, while an overlapping
        // padded alias must receive the rendered pixels through
        // synchronization.
        metal::Renderer range_renderer;
        auto& range_cache = range_renderer.getTextureCache();
        commands.reset(range_renderer.createCommandBuffer());
        auto target_desc =
            TextureDescriptor(ptr, TextureType::_2D, TextureFormat::RGBA8Unorm,
                              false, 0, 16, 16, 1, 1, 1, 0, 0, 0);
        auto alias_desc =
            TextureDescriptor(ptr, TextureType::_2D, TextureFormat::RGBA8Unorm,
                              false, 0, 8, 16, 1, 1, 1, 0, 0, 0);
        auto separate_desc = TextureDescriptor(ptr + 4096, TextureType::_2D,
                                               TextureFormat::RGBA8Unorm, false,
                                               0, 16, 16, 1, 1, 1, 0, 0, 0);
        auto* target_view =
            range_cache.find(commands.get(), target_desc, TextureUsage::Read);
        auto* alias_view =
            range_cache.find(commands.get(), alias_desc, TextureUsage::Read);
        auto* separate_view =
            range_cache.find(commands.get(), separate_desc, TextureUsage::Read);
        check(target_view->getBase() != alias_view->getBase(),
              "Padded alias exercises distinct storages");
        auto* target = static_cast<metal::Texture*>(target_view->getBase());
        range_cache.find(commands.get(), target_desc, TextureUsage::Write);
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* attachment = pass->colorAttachments()->object(0);
        attachment->setTexture(target->getTexture());
        attachment->setLoadAction(MTL::LoadActionClear);
        attachment->setStoreAction(MTL::StoreActionStore);
        attachment->setClearColor(MTL::ClearColor(1, 0, 0, 1));
        static_cast<metal::CommandBuffer*>(commands.get())
            ->getRenderCommandEncoder(pass);
        auto bridge =
            TextureDescriptor(ptr, TextureType::_2D, TextureFormat::RGBA8Unorm,
                              true, 256, 64, 64, 1, 1, 1, 0, 0, 0);
        range_cache.find(commands.get(), bridge, TextureUsage::Read);
        check(range_cache.getMemoryCount() == 1,
              "Bridge merges written and sampled memory intervals");
        auto& stats = hw::tegra_x1::gpu::PerfStats::instance();
        const auto skips = stats.texture_sync_skips.load();
        range_cache.find(commands.get(), separate_desc, TextureUsage::Read);
        if (std::getenv("HYDRA_DISABLE_TEXTURE_WRITE_FILTER") == nullptr)
            check(stats.texture_sync_skips.load() > skips,
                  "Disjoint texture avoids full synchronization scan");
        const auto scans = stats.texture_sync_scans.load();
        range_cache.find(commands.get(), alias_desc, TextureUsage::Read);
        check(stats.texture_sync_scans.load() > scans,
              "Overlapping alias still scans for GPU writes after merge");
        // CPU invalidation must still upload a disjoint sampled texture.
        std::memset(guest_memory.data() + 4096, 0x7a, separate_desc.size);
        range_cache.invalidateMemory(separate_desc.getRange());
        range_cache.find(commands.get(), separate_desc, TextureUsage::Read);
        auto* pixels = range_renderer.getDevice()->newBuffer(
            512, MTL::ResourceStorageModeShared);
        auto* blit = static_cast<metal::CommandBuffer*>(commands.get())
                         ->getBlitCommandEncoder();
        auto read_pixel = [&](ITextureView* view, usize offset) {
            auto* texture = static_cast<metal::Texture*>(view->getBase());
            blit->copyFromTexture(texture->getTexture(), 0, 0,
                                  MTL::Origin(0, 0, 0), MTL::Size(1, 1, 1),
                                  pixels, offset, 256, 256);
        };
        read_pixel(alias_view, 0);
        read_pixel(separate_view, 256);
        commands.reset();
        const auto* rgba = static_cast<const u8*>(pixels->contents());
        const bool correct = rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 &&
                             rgba[3] == 255 && rgba[256] == 0x7a &&
                             rgba[259] == 0x7a;
        pixels->release();
        check(correct, "GPU alias copy and CPU invalidation preserve pixels");

        // A compute-written buffer subrange must update a wider sampled alias.
        // CPU memory remains zero, so re-uploading it would erase the GPU data.
        std::vector<u32> buffer_guest(128, 0);
        const auto buffer_ptr = reinterpret_cast<uptr>(buffer_guest.data());
        metal::Renderer buffer_renderer;
        auto& buffer_cache = buffer_renderer.getTextureCache();
        commands.reset(buffer_renderer.createCommandBuffer());
        auto buffer_full = TextureDescriptor(buffer_ptr, TextureType::_1DBuffer,
                                             TextureFormat::R32Uint, true, 256,
                                             64, 1, 1, 1, 1, 0, 0, 0);
        auto buffer_sub = TextureDescriptor(
            buffer_ptr + 64, TextureType::_1DBuffer, TextureFormat::R32Uint,
            true, 128, 32, 1, 1, 1, 1, 0, 0, 0);
        auto* full_view =
            buffer_cache.find(commands.get(), buffer_full, TextureUsage::Read);
        buffer_cache.invalidateMemory(buffer_full.getRange());
        auto* sub_view =
            buffer_cache.find(commands.get(), buffer_sub, TextureUsage::Write);
        const char* buffer_source = R"(
            #include <metal_stdlib>
            using namespace metal;
            kernel void fill(texture_buffer<uint, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) { t.write(uint4(0xdead0000u+i), i); }
            kernel void fill_half(texture_buffer<uint, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) { t.write(uint4(0x1200u+i), i); }
            kernel void readback(texture_buffer<uint, access::read> t [[texture(0)]], device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i]=t.read(i).x; }
        )";
        NS::Error* buffer_error = nullptr;
        NS_STACK_SCOPED auto buffer_library =
            buffer_renderer.getDevice()->newLibrary(
                metal::toNsString(buffer_source), nullptr, &buffer_error);
        check(buffer_library != nullptr, "Buffer alias shaders compile");
        NS_STACK_SCOPED auto fill_function =
            buffer_library->newFunction(metal::toNsString("fill"));
        NS_STACK_SCOPED auto read_function =
            buffer_library->newFunction(metal::toNsString("readback"));
        NS_STACK_SCOPED auto fill_pipeline =
            buffer_renderer.getDevice()->newComputePipelineState(fill_function,
                                                                 &buffer_error);
        NS_STACK_SCOPED auto read_pipeline =
            buffer_renderer.getDevice()->newComputePipelineState(read_function,
                                                                 &buffer_error);
        NS_STACK_SCOPED auto half_function =
            buffer_library->newFunction(metal::toNsString("fill_half"));
        NS_STACK_SCOPED auto half_pipeline =
            buffer_renderer.getDevice()->newComputePipelineState(half_function,
                                                                 &buffer_error);
        check(half_pipeline, "R16 fill pipeline compiles");
        check(fill_pipeline && read_pipeline, "Buffer alias pipelines compile");
        auto* buffer_encoder =
            static_cast<metal::CommandBuffer*>(commands.get())
                ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(fill_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(sub_view->getBase())->getTexture(), 0);
        buffer_encoder->dispatchThreads(MTL::Size(32, 1, 1),
                                        MTL::Size(32, 1, 1));
        buffer_cache.find(commands.get(), buffer_full, TextureUsage::Read);
        // Also create an alias after the GPU write. It has no earlier cached
        // contents and must recover the written data rather than stale CPU
        // zeros.
        auto late_descriptor = TextureDescriptor(
            buffer_ptr + 80, TextureType::_1DBuffer, TextureFormat::R32Uint,
            true, 64, 16, 1, 1, 1, 1, 0, 0, 0);
        auto* late_view = buffer_cache.find(commands.get(), late_descriptor,
                                            TextureUsage::Read);
        NS_STACK_SCOPED auto late_pixels =
            buffer_renderer.getDevice()->newBuffer(
                64, MTL::ResourceStorageModeShared);
        buffer_encoder = static_cast<metal::CommandBuffer*>(commands.get())
                             ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(read_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(late_view->getBase())->getTexture(),
            0);
        buffer_encoder->setBuffer(late_pixels, 0, 0);
        buffer_encoder->dispatchThreads(MTL::Size(16, 1, 1),
                                        MTL::Size(16, 1, 1));
        NS_STACK_SCOPED auto buffer_pixels =
            buffer_renderer.getDevice()->newBuffer(
                256, MTL::ResourceStorageModeShared);
        buffer_encoder = static_cast<metal::CommandBuffer*>(commands.get())
                             ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(read_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(full_view->getBase())->getTexture(),
            0);
        buffer_encoder->setBuffer(buffer_pixels, 0, 0);
        buffer_encoder->dispatchThreads(MTL::Size(64, 1, 1),
                                        MTL::Size(32, 1, 1));
        // Reinterpret the written bytes as R16, starting at a half-word
        // boundary. Copying texel indices or requiring equal formats loses
        // data.
        auto half_descriptor = TextureDescriptor(
            buffer_ptr + 82, TextureType::_1DBuffer, TextureFormat::R16Uint,
            true, 62, 31, 1, 1, 1, 1, 0, 0, 0);
        auto* half_view = buffer_cache.find(commands.get(), half_descriptor,
                                            TextureUsage::Read);
        NS_STACK_SCOPED auto half_pixels =
            buffer_renderer.getDevice()->newBuffer(
                31 * sizeof(u32), MTL::ResourceStorageModeShared);
        buffer_encoder = static_cast<metal::CommandBuffer*>(commands.get())
                             ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(read_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(half_view->getBase())->getTexture(),
            0);
        buffer_encoder->setBuffer(half_pixels, 0, 0);
        buffer_encoder->dispatchThreads(MTL::Size(31, 1, 1),
                                        MTL::Size(31, 1, 1));
        commands.reset();
        const auto* buffer_result =
            static_cast<const u32*>(buffer_pixels->contents());
        for (u32 i = 0; i < 64; ++i)
            check(buffer_result[i] ==
                      (i >= 16 && i < 48 ? 0xdead0000u + i - 16 : 0u),
                  "GPU buffer alias preserves both written subrange and "
                  "untouched texels");
        const auto* late_result =
            static_cast<const u32*>(late_pixels->contents());
        for (u32 i = 0; i < 16; ++i)
            check(late_result[i] == 0xdead0004u + i,
                  "New buffer alias preserves earlier GPU writes");
        const auto* half_result =
            static_cast<const u32*>(half_pixels->contents());
        for (u32 i = 0; i < 31; ++i) {
            const u32 byte_offset = 18 + i * 2;
            const u32 word = 0xdead0000u + byte_offset / 4;
            check(half_result[i] ==
                      ((word >> ((byte_offset % 4) * 8)) & 0xffffu),
                  "R16 alias preserves GPU R32 bytes at half-word offset");
        }
        std::cout
            << "PASS: GPU-written buffer subrange synchronizes to sampled alias"
            << std::endl;

        // Write through R16, then create a fresh R32 alias. Both the older
        // R32 source and the newer R16 source overlap: latest bytes must win.
        commands.reset(buffer_renderer.createCommandBuffer());
        buffer_cache.find(commands.get(), half_descriptor, TextureUsage::Write);
        buffer_encoder = static_cast<metal::CommandBuffer*>(commands.get())
                             ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(half_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(half_view->getBase())->getTexture(),
            0);
        buffer_encoder->dispatchThreads(MTL::Size(31, 1, 1),
                                        MTL::Size(31, 1, 1));
        auto mixed_descriptor = TextureDescriptor(
            buffer_ptr + 80, TextureType::_1DBuffer, TextureFormat::R32Uint,
            true, 80, 20, 1, 1, 1, 1, 0, 0, 0);
        auto* mixed_view = buffer_cache.find(commands.get(), mixed_descriptor,
                                             TextureUsage::Read);
        NS_STACK_SCOPED auto mixed_pixels =
            buffer_renderer.getDevice()->newBuffer(
                80, MTL::ResourceStorageModeShared);
        buffer_encoder = static_cast<metal::CommandBuffer*>(commands.get())
                             ->getComputeCommandEncoder();
        buffer_encoder->setComputePipelineState(read_pipeline);
        buffer_encoder->setTexture(
            static_cast<metal::Texture*>(mixed_view->getBase())->getTexture(),
            0);
        buffer_encoder->setBuffer(mixed_pixels, 0, 0);
        buffer_encoder->dispatchThreads(MTL::Size(20, 1, 1),
                                        MTL::Size(20, 1, 1));
        commands.reset();
        std::array<u32, 32> expected_words{};
        for (u32 i = 0; i < 32; ++i)
            expected_words[i] = 0xdead0000u + i;
        auto* expected_bytes = reinterpret_cast<u8*>(expected_words.data());
        for (u32 i = 0; i < 31; ++i) {
            const u16 half = static_cast<u16>(0x1200 + i);
            std::memcpy(expected_bytes + 18 + i * 2, &half, 2);
        }
        const auto* mixed_result =
            static_cast<const u32*>(mixed_pixels->contents());
        for (u32 i = 0; i < 20; ++i)
            check(mixed_result[i] == expected_words[i + 4],
                  "R32 alias preserves newer R16 writes and older neighboring "
                  "bytes");
        std::cout << "PASS: Cross-format buffer aliases preserve bytes and GPU "
                     "write order"
                  << std::endl;

        // Crash 4 samples a GPU-written Z24S8 target using a G24R8 TIC.
        // Guest memory deliberately contains zero depth: sampling it instead
        // of the rendered surface would fail this regression.
        using namespace hw::tegra_x1::gpu;
        ImageFormatWord depth_tic{};
        depth_tic.image_format = ImageFormat::G24R8;
        depth_tic.component_r = ImageComponent::Uint;
        depth_tic.component_g = depth_tic.component_b = depth_tic.component_a =
            ImageComponent::Unorm;
        auto sampled_format = toTextureFormat(depth_tic, false);
        check(sampled_format == toTextureFormat(DepthSurfaceFormat::Z24S8Unorm),
              "G24R8 sampling aliases Z24S8 render target format");
        auto depth_desc =
            TextureDescriptor(ptr + 16384, TextureType::_2D, sampled_format,
                              false, 0, 16, 16, 1, 1, 1, 0, 0, 0);
        commands.reset(range_renderer.createCommandBuffer());
        auto* depth_target =
            range_cache.find(commands.get(), depth_desc, TextureUsage::Write);
        auto* depth_native =
            static_cast<metal::Texture*>(depth_target->getBase())->getTexture();
        auto* depth_pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        depth_pass->depthAttachment()->setTexture(depth_native);
        depth_pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        depth_pass->depthAttachment()->setStoreAction(MTL::StoreActionStore);
        depth_pass->depthAttachment()->setClearDepth(0.625);
        static_cast<metal::CommandBuffer*>(commands.get())
            ->getRenderCommandEncoder(depth_pass);
        SwizzleChannels depth_swizzle(sampled_format, ImageSwizzle::G,
                                      ImageSwizzle::G, ImageSwizzle::G,
                                      ImageSwizzle::OneFloat);
        auto* sampled_depth = range_cache.find(
            commands.get(), depth_desc,
            TextureViewDescriptor(TextureType::_2D, sampled_format,
                                  ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1),
                                  depth_swizzle),
            TextureUsage::Read);
        check(sampled_depth->getBase() == depth_target->getBase(),
              "Depth sampling reuses GPU-written storage");
        auto* sample_native =
            static_cast<metal::TextureView*>(sampled_depth)->getTexture();
        check(sample_native != nullptr, "Depth sampling view is valid");
        NS::Error* error = nullptr;
        auto* device = range_renderer.getDevice();
        auto* library =
            device->newLibrary(NS::String::string(R"(
            #include <metal_stdlib>
            using namespace metal;
            kernel void sample_depth(texture2d<float, access::read> depth [[texture(0)]],
                                     device float4* result [[buffer(0)]]) {
                result[0] = depth.read(uint2(0));
            }
        )",
                                                  NS::UTF8StringEncoding),
                               nullptr, &error);
        check(library != nullptr, "Depth sampling shader compiles");
        auto* function = library->newFunction(
            NS::String::string("sample_depth", NS::UTF8StringEncoding));
        auto* pipeline = device->newComputePipelineState(function, &error);
        check(pipeline != nullptr, "Depth sampling pipeline compiles");
        auto* depth_result =
            device->newBuffer(16, MTL::ResourceStorageModeShared);
        auto* compute = static_cast<metal::CommandBuffer*>(commands.get())
                            ->getComputeCommandEncoder();
        compute->setComputePipelineState(pipeline);
        compute->setTexture(sample_native, 0);
        compute->setBuffer(depth_result, 0, 0);
        compute->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        commands.reset();
        const auto* result =
            static_cast<const float*>(depth_result->contents());
        check(std::abs(result[0] - 0.625f) < 0.0001f &&
                  std::abs(result[1] - 0.625f) < 0.0001f &&
                  std::abs(result[2] - 0.625f) < 0.0001f && result[3] == 1.0f,
              "G24R8 samples rendered depth through G and preserves constants");
        depth_result->release();
        pipeline->release();
        function->release();
        library->release();
        std::cout << "PASS: G24R8 alias samples GPU-written Z24S8 depth\n";

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

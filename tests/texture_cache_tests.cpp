#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
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

void runTextureFormatTests();

int main() {
    try {
        // Exercise the real cache and Metal texture views, including
        // overlapping guest memory and contained mip levels from the CTR crash
        // path.
        setenv("HYDRA_GPU_SYNC", "1", 1);
        setenv("HYDRA_PERF_STATS", "1", 1);
        setenv("HYDRA_DISABLE_SHADER_CACHE", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        runTextureFormatTests();
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

        // A mode/allocator transition may reuse an address with a different
        // tiling or pitch. Equal dimensions must not reuse decoded pixels.
        for (u32 variant = 0; variant < 3; ++variant) {
            std::vector<u8> layout_memory(65536);
            for (usize i = 0; i < layout_memory.size(); ++i)
                layout_memory[i] = static_cast<u8>((i * 37 + (i >> 5) * 13 +
                                                    (i >> 9) * 29) & 255);
            metal::Renderer layout_renderer;
            auto& layouts = layout_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> cb(layout_renderer.createCommandBuffer());
            const auto address = reinterpret_cast<uptr>(layout_memory.data());
            const TextureDescriptor first(address, TextureType::_2D,
                TextureFormat::RGBA8Unorm, variant == 1, variant == 1 ? 256 : 0,
                32, 32, 1, 1, 1, 0, 0, 0);
            const TextureDescriptor second(address, TextureType::_2D,
                TextureFormat::RGBA8Unorm, variant != 0, variant != 0 ? 128 : 0,
                32, 32, 1, 1, 1, 0, variant == 0 ? 2 : 0, 0);
            auto* a = layouts.find(cb.get(), first, TextureUsage::Read);
            auto* b = layouts.find(cb.get(), second, TextureUsage::Read);
            check(a->getBase() != b->getBase(),
                  "Different tiling/pitch at the same address has separate storage");
            NS_STACK_SCOPED auto output = layout_renderer.getDevice()->newBuffer(
                16384, MTL::ResourceStorageModeShared);
            auto* blit = static_cast<metal::CommandBuffer*>(cb.get())->getBlitCommandEncoder();
            for (u32 index = 0; index < 2; ++index) {
                auto* view = index == 0 ? a : b;
                blit->copyFromTexture(static_cast<metal::Texture*>(view->getBase())->getTexture(),
                    0, 0, MTL::Origin(0, 0, 0), MTL::Size(32, 32, 1),
                    output, index * 8192, 256, 8192);
            }
            cb.reset();
            const auto* bytes = static_cast<const u8*>(output->contents());
            for (u32 index = 0; index < 2; ++index) {
                const auto& desc = index == 0 ? first : second;
                for (u32 y = 0; y < 32; ++y)
                    for (u32 x = 0; x < 128; ++x) {
                        const auto offset = desc.is_linear
                            ? u64(y) * desc.linear_stride + x
                            : hw::tegra_x1::gpu::getBlockLinearOffset(
                                  x, y, 0, 128, 32, desc.block_height_gobs_log2, 0);
                        check(bytes[index * 8192 + y * 256 + x] == layout_memory[offset],
                              "Each cached layout decodes its own guest memory pixels");
                    }
            }
        }
        std::cout << "PASS: address reuse with changed block height, pitch and linear/tiled layout\n";

        // UE4 renders each level of a reflection capture cubemap separately.
        // The base cubemap describes one level, but its layer stride holds the
        // whole chain, so later levels must become views of the same storage.
        {
            std::vector<u8> cube_memory(0x110000, 0);
            const auto cube_ptr = reinterpret_cast<uptr>(cube_memory.data());
            std::unique_ptr<ICommandBuffer> cube_commands(
                renderer.createCommandBuffer());
            const TextureDescriptor cube(cube_ptr, TextureType::_2DArray,
                                         TextureFormat::RGBA16Float, false, 0,
                                         128, 128, 1, 1, 6, 0, 4, 0, 0x2c000);
            auto* cube_view =
                cache.find(cube_commands.get(), cube, TextureUsage::Write);
            // Render to face 3 of level 0 before the storage has to grow
            {
                auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
                auto* attachment = pass->colorAttachments()->object(0);
                attachment->setTexture(
                    static_cast<metal::Texture*>(cube_view->getBase())
                        ->getTexture());
                attachment->setSlice(3);
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearColor(MTL::ClearColor(0.5, 0.25, 2.0, 1));
                static_cast<metal::CommandBuffer*>(cube_commands.get())
                    ->getRenderCommandEncoder(pass);
            }
            const TextureDescriptor level1(
                cube_ptr + 0x20000, TextureType::_2DArray,
                TextureFormat::RGBA16Float, false, 0, 64, 64, 1, 1, 6, 0, 3, 0,
                0x2c000);
            auto* level1_view =
                cache.find(cube_commands.get(), level1, TextureUsage::Write);
            check(level1_view->getBase()->getDescriptor().ptr == cube_ptr,
                  "Cubemap level shares the cubemap storage");
            check(level1_view->getBase()->getDescriptor().level_count >= 2,
                  "Cubemap storage grows to hold the rendered level");
            check(level1_view->getDescriptor().levels.getBegin() == 1,
                  "Rendered level is a view of level 1");
            const TextureDescriptor full(cube_ptr, TextureType::_2DArray,
                                         TextureFormat::RGBA16Float, false, 0,
                                         128, 128, 1, 2, 6, 0, 4, 0, 0x2c000);
            auto* full_view =
                cache.find(cube_commands.get(), full, TextureUsage::Read);
            check(full_view->getBase() == level1_view->getBase(),
                  "Sampling the whole chain reuses the same storage");
            auto* readback = renderer.getDevice()->newBuffer(
                8, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(cube_commands.get())
                ->getBlitCommandEncoder()
                ->copyFromTexture(
                    static_cast<metal::Texture*>(full_view->getBase())
                        ->getTexture(),
                    3, 0, MTL::Origin(5, 5, 0), MTL::Size(1, 1, 1), readback,
                    0, 8, 8);
            cube_commands.reset();
            const auto* half = static_cast<const _Float16*>(readback->contents());
            const bool kept = static_cast<f32>(half[0]) == 0.5f &&
                              static_cast<f32>(half[1]) == 0.25f &&
                              static_cast<f32>(half[2]) == 2.0f;
            readback->release();
            check(kept, "Growing the cubemap keeps already rendered levels");

            // Small levels are rendered one face at a time with a padded width,
            // e.g. level 5 (4x4) of face 4 as an 8x4 render target
            std::unique_ptr<ICommandBuffer> sky_commands(
                renderer.createCommandBuffer());
            const TextureDescriptor sky(cube_ptr, TextureType::_2DArray,
                                        TextureFormat::RGBA16Float, false, 0,
                                        128, 128, 1, 8, 6, 0, 4, 0, 0x2c000);
            auto* sky_view =
                cache.find(sky_commands.get(), sky, TextureUsage::Read);
            const TextureDescriptor padded(
                cube_ptr + 4 * 0x2c000 + sky.getLevelOffset(5),
                TextureType::_2D, TextureFormat::RGBA16Float, false, 0, 8, 4,
                1, 1, 1, 0, 0, 0);
            auto* padded_view =
                cache.find(sky_commands.get(), padded, TextureUsage::Write);
            check(padded_view->getBase() == sky_view->getBase(),
                  "Padded small level render target shares the cubemap");
            check(padded_view->getDescriptor().levels.getBegin() == 5 &&
                      padded_view->getDescriptor().layers.getBegin() == 4,
                  "Padded render target views the right level and face");
        }

        // An alias can start at a nonzero mip and layer yet extend beyond the
        // original storage, so the cache must synchronize separate textures.
        // Mip and layer are deliberately different from the destination's.
        {
            std::vector<u8> alias_memory(0x10000, 0);
            const auto alias_ptr = reinterpret_cast<uptr>(alias_memory.data());
            metal::Renderer alias_renderer;
            auto& alias_cache = alias_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> alias_commands(
                alias_renderer.createCommandBuffer());
            const TextureDescriptor source(
                alias_ptr, TextureType::_2DArray, TextureFormat::RGBA8Unorm,
                false, 0, 32, 32, 1, 3, 2, 0, 0, 0, 0x2000);
            const TextureDescriptor destination(
                alias_ptr + source.layer_size + source.getLevelOffset(1),
                TextureType::_2DArray, TextureFormat::RGBA8Unorm, false, 0,
                16, 16, 1, 2, 2, 0, 0, 0, 0x2000);
            auto* source_view = alias_cache.find(
                alias_commands.get(), source, TextureUsage::Read);
            auto* destination_view = alias_cache.find(
                alias_commands.get(), destination, TextureUsage::Read);
            check(source_view->getBase() != destination_view->getBase(),
                  "Extending layered alias keeps separate storages");
            alias_cache.find(alias_commands.get(), source, TextureUsage::Write);
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(
                static_cast<metal::Texture*>(source_view->getBase())
                    ->getTexture());
            attachment->setLevel(1);
            attachment->setSlice(1);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor(1, 0, 0, 1));
            static_cast<metal::CommandBuffer*>(alias_commands.get())
                ->getRenderCommandEncoder(pass);
            destination_view = alias_cache.find(
                alias_commands.get(), destination, TextureUsage::Read);
            NS_STACK_SCOPED auto readback =
                alias_renderer.getDevice()->newBuffer(
                    512, MTL::ResourceStorageModeShared);
            auto* blit = static_cast<metal::CommandBuffer*>(alias_commands.get())
                             ->getBlitCommandEncoder();
            auto* texture =
                static_cast<metal::Texture*>(destination_view->getBase())
                    ->getTexture();
            for (u32 layer = 0; layer < 2; ++layer)
                blit->copyFromTexture(texture, layer, 0, MTL::Origin(0, 0, 0),
                                      MTL::Size(1, 1, 1), readback, layer * 256,
                                      256, 256);
            alias_commands.reset();
            const auto* rgba = static_cast<const u8*>(readback->contents());
            check(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 &&
                      rgba[3] == 255,
                  "Alias receives source mip 1 layer 1 in mip 0 layer 0");
            check(rgba[256] == 0 && rgba[259] == 0,
                  "Alias synchronization preserves the unrelated layer");
            std::cout << "PASS: nonzero mip/layer alias synchronization\n";
        }

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

        // Page-granular invalidation can include constants that did not change.
        // Reuse identical immutable snapshots, but preserve old GPU reads when
        // an actual CPU update requires a new snapshot.
        {
            std::array<u32, 64> constants{};
            constants.fill(0x12345678);
            const auto address = reinterpret_cast<uptr>(constants.data());
            const auto range =
                ztd::Range<uptr>::fromSize(address, sizeof(constants));
            const auto changed =
                ztd::Range<uptr>::fromSize(address + 16, sizeof(u32));
            auto& buffers = renderer.getBufferCache();
            commands.reset(renderer.createCommandBuffer());
            auto first = buffers.get(commands.get(), range);
            buffers.invalidateMemory(changed);
            auto identical = buffers.get(commands.get(), range);
            check(first.getBase() == identical.getBase(),
                  "Unchanged invalidated constants reuse their snapshot");
            constants[4] = 0xdeadbeef;
            buffers.invalidateMemory(changed);
            const auto updated = buffers.get(commands.get(), range);
            check(first.getBase() != updated.getBase(),
                  "Changed constants receive a new immutable snapshot");
            NS_STACK_SCOPED auto result = renderer.getDevice()->newBuffer(
                2 * sizeof(constants), MTL::ResourceStorageModeShared);
            auto* snapshot_blit =
                static_cast<metal::CommandBuffer*>(commands.get())
                    ->getBlitCommandEncoder();
            snapshot_blit->copyFromBuffer(
                static_cast<metal::Buffer*>(first.getBase())->getBuffer(), 0,
                result, 0, sizeof(constants));
            snapshot_blit->copyFromBuffer(
                static_cast<metal::Buffer*>(updated.getBase())->getBuffer(), 0,
                result, sizeof(constants), sizeof(constants));
            // Drop CPU views while GPU reads are still pending. Recycling must
            // wait for this command buffer, rather than overwrite the old data.
            hydra::hw::tegra_x1::gpu::tls_crnt_command_buffer = commands.get();
            first = BufferView{};
            identical = BufferView{};
            hydra::hw::tegra_x1::gpu::tls_crnt_command_buffer = nullptr;
            commands.reset();
            const auto* values = static_cast<const u32*>(result->contents());
            check(values[4] == 0x12345678 && values[64 + 4] == 0xdeadbeef,
                  "GPU sees both old and updated constant snapshots");
            const auto reuse_before =
                hydra::hw::tegra_x1::gpu::PerfStats::instance()
                    .upload_buffers_reused.load();
            constants[4] = 0xcafebabe;
            buffers.invalidateMemory(changed);
            commands.reset(renderer.createCommandBuffer());
            const auto reused = buffers.get(commands.get(), range);
            check(hydra::hw::tegra_x1::gpu::PerfStats::instance()
                          .upload_buffers_reused.load() > reuse_before,
                  "Completed snapshot allocation is reused by a later update");
            check(*reinterpret_cast<const u32*>(reused.getPtr() + 16) ==
                      0xcafebabe,
                  "Reused snapshot contains the new CPU data");
            commands.reset();
            std::cout << "PASS: unchanged buffer invalidation, deferred "
                         "snapshot reuse and isolated GPU reads\n";
        }

        // GPU data uses raw guest byte layout even when the other view is RG32F.
        for (const bool linear_surface : {false, true}) {
            std::vector<u8> alias_guest(8192, 0);
            const uptr address = reinterpret_cast<uptr>(alias_guest.data());
            metal::Renderer aliases_renderer;
            auto& aliases = aliases_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> cb(aliases_renderer.createCommandBuffer());
            const TextureDescriptor raw_desc(address, TextureType::_1DBuffer,
                TextureFormat::R32Uint, true, 8192, 2048, 1, 1, 1, 1, 0, 0, 0);
            const u32 height = linear_surface ? 3 : 16;
            const TextureDescriptor surface_desc(address + 512, TextureType::_2D,
                TextureFormat::RG32Float, linear_surface, linear_surface ? 160 : 0,
                16, height, 1, 1, 1, 0, 1, 0);
            auto* surface = aliases.find(cb.get(), surface_desc, TextureUsage::Read);
            auto* raw = aliases.find(cb.get(), raw_desc, TextureUsage::Write);
            auto* raw_native = static_cast<metal::Texture*>(raw->getBase())->getTexture();
            auto* surface_native = static_cast<metal::Texture*>(surface->getBase())->getTexture();
            NS_STACK_SCOPED auto upload = aliases_renderer.getDevice()->newBuffer(8192, MTL::ResourceStorageModeShared);
            NS_STACK_SCOPED auto packed_readback = aliases_renderer.getDevice()->newBuffer(128 * height, MTL::ResourceStorageModeShared);
            NS_STACK_SCOPED auto raw_readback = aliases_renderer.getDevice()->newBuffer(8192, MTL::ResourceStorageModeShared);
            std::vector<u8> pattern(8192);
            for (usize i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<u8>((i * 71 + 19) & 255);
            const u32 nan_payload = 0x7fc12345;
            std::memcpy(pattern.data() + 512, &nan_payload, sizeof(nan_payload));
            std::memcpy(upload->contents(), pattern.data(), pattern.size());
            const auto commands_impl = [&] { return static_cast<metal::CommandBuffer*>(cb.get()); };
            commands_impl()->getBlitCommandEncoder()->copyFromBuffer(upload, 0, raw_native->buffer(), 0, 8192);
            const auto guest_offset = [&](u32 x, u32 y) -> u64 {
                if (linear_surface) return u64(y) * 160 + x;
                const auto blocks = surface_desc.getLevelBlockSizeLog2(0);
                return hydra::hw::tegra_x1::gpu::getBlockLinearOffset(x, y, 0, 128, height, blocks.y(), blocks.z());
            };
            const auto read_surface = [&] {
                commands_impl()->getBlitCommandEncoder()->copyFromTexture(surface_native, 0, 0,
                    MTL::Origin(0, 0, 0), MTL::Size(16, height, 1), packed_readback, 0, 128, 128 * height);
                cb.reset();
                cb.reset(aliases_renderer.createCommandBuffer());
            };
            // A partial, byte-unaligned update preserves untouched logical texels.
            surface->getBase()->copyBufferBytes(cb.get(), raw->getBase(), 512 + 17, 17, 91);
            read_surface();
            const auto* packed_bytes = static_cast<const u8*>(packed_readback->contents());
            for (u32 y = 0; y < height; ++y)
                for (u32 x = 0; x < 128; ++x) {
                    const auto offset = guest_offset(x, y);
                    const u8 expected = offset >= 17 && offset < 108 ? pattern[512 + offset] : 0;
                    check(packed_bytes[y * 128 + x] == expected, "Mixed alias partial copy preserves other texels");
                }
            // Real cache lookup must synchronize the full newer GPU buffer contents.
            aliases.find(cb.get(), surface_desc, TextureUsage::Read);
            read_surface();
            packed_bytes = static_cast<const u8*>(packed_readback->contents());
            for (u32 y = 0; y < height; ++y)
                for (u32 x = 0; x < 128; ++x)
                    check(packed_bytes[y * 128 + x] == pattern[512 + guest_offset(x, y)],
                          "GPU buffer bytes reach the sampled 2D alias without float conversion");
            check(std::all_of(alias_guest.begin(), alias_guest.end(), [](u8 value) { return value == 0; }),
                  "Mixed alias synchronization does not modify guest memory");
            // Write the 2D surface on the GPU and synchronize back into the buffer.
            aliases.find(cb.get(), surface_desc, TextureUsage::Write);
            auto* packed_upload = static_cast<u8*>(upload->contents());
            for (u32 i = 0; i < 128 * height; ++i) packed_upload[i] = static_cast<u8>((i * 29 + 53) & 255);
            commands_impl()->getBlitCommandEncoder()->copyFromBuffer(upload, 0, 128, 128 * height,
                MTL::Size(16, height, 1), surface_native, 0, 0, MTL::Origin(0, 0, 0));
            aliases.find(cb.get(), raw_desc, TextureUsage::Read);
            commands_impl()->getBlitCommandEncoder()->copyFromBuffer(raw_native->buffer(), 0, raw_readback, 0, 8192);
            cb.reset();
            for (u32 y = 0; y < height; ++y)
                for (u32 x = 0; x < 128; ++x)
                    pattern[512 + guest_offset(x, y)] = static_cast<u8>(((y * 128 + x) * 29 + 53) & 255);
            check(std::memcmp(raw_readback->contents(), pattern.data(), pattern.size()) == 0,
                  "2D-to-buffer alias preserves pitch padding and neighboring guest bytes");
        }
        std::cout << "PASS: bidirectional pitch/tiled 2D-buffer aliases, raw float bits and partial byte copies\n";

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
        // Both keep the stencil in the low byte (NVIDIA's ZT format Z24S8)
        check(sampled_format == toTextureFormat(DepthSurfaceFormat::S8Z24Unorm),
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
        // A render target with the other Z24 layout (ZT S8Z24) shares the
        // storage, both are Depth32Float_Stencil8 on the host
        auto other_layout_desc = depth_desc;
        other_layout_desc.format =
            toTextureFormat(DepthSurfaceFormat::Z24S8Unorm);
        check(other_layout_desc.format != sampled_format,
              "The two Z24 layouts are distinct formats");
        check(range_cache
                      .find(commands.get(), other_layout_desc,
                            TextureUsage::Read)
                      ->getBase() == depth_target->getBase(),
              "Z24 layouts alias the same depth storage");
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

        // UE4 copies the scene stencil with an RRRR swizzle on the same TIC
        // and reads the raw stencil bits. Returning the depth plane instead
        // produced depth-dependent stripes in Crash 4's lighting.
        ImageFormatWord stencil_tic = depth_tic;
        stencil_tic.swizzle_x = stencil_tic.swizzle_y =
            stencil_tic.swizzle_z = stencil_tic.swizzle_w = ImageSwizzle::R;
        const auto stencil_format = toSampledTextureFormat(stencil_tic, false);
        check(stencil_format == TextureFormat::S8Uint,
              "Z24S8 TIC reading only R samples the stencil plane");
        check(toSampledTextureFormat(depth_tic, false) == sampled_format,
              "Z24S8 TIC reading G samples the depth plane");
        commands.reset(range_renderer.createCommandBuffer());
        depth_target =
            range_cache.find(commands.get(), depth_desc, TextureUsage::Write);
        depth_native =
            static_cast<metal::Texture*>(depth_target->getBase())->getTexture();
        auto* stencil_pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        stencil_pass->depthAttachment()->setTexture(depth_native);
        stencil_pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        stencil_pass->depthAttachment()->setStoreAction(MTL::StoreActionStore);
        stencil_pass->depthAttachment()->setClearDepth(0.625);
        stencil_pass->stencilAttachment()->setTexture(depth_native);
        stencil_pass->stencilAttachment()->setLoadAction(MTL::LoadActionClear);
        stencil_pass->stencilAttachment()->setStoreAction(
            MTL::StoreActionStore);
        stencil_pass->stencilAttachment()->setClearStencil(0x5a);
        static_cast<metal::CommandBuffer*>(commands.get())
            ->getRenderCommandEncoder(stencil_pass);
        auto* sampled_stencil = range_cache.find(
            commands.get(), depth_desc,
            TextureViewDescriptor(
                TextureType::_2D, stencil_format, ztd::Range<u32>(0, 1),
                ztd::Range<u32>(0, 1),
                SwizzleChannels(stencil_format, ImageSwizzle::R,
                                ImageSwizzle::R, ImageSwizzle::R,
                                ImageSwizzle::R)),
            TextureUsage::Read);
        check(sampled_stencil->getBase() == depth_target->getBase(),
              "Stencil sampling reuses GPU-written storage");
        auto* stencil_native =
            static_cast<metal::TextureView*>(sampled_stencil)->getTexture();
        check(stencil_native != nullptr, "Stencil sampling view is valid");
        auto* stencil_library = device->newLibrary(
            NS::String::string(R"(
            #include <metal_stdlib>
            using namespace metal;
            kernel void sample_stencil(texture2d<uint, access::read> stencil [[texture(0)]],
                                       device uint4* result [[buffer(0)]]) {
                result[0] = stencil.read(uint2(0));
            }
        )",
                               NS::UTF8StringEncoding),
            nullptr, &error);
        check(stencil_library != nullptr, "Stencil sampling shader compiles");
        auto* stencil_function = stencil_library->newFunction(
            NS::String::string("sample_stencil", NS::UTF8StringEncoding));
        auto* stencil_pipeline =
            device->newComputePipelineState(stencil_function, &error);
        check(stencil_pipeline != nullptr, "Stencil sampling pipeline compiles");
        auto* stencil_result =
            device->newBuffer(16, MTL::ResourceStorageModeShared);
        compute = static_cast<metal::CommandBuffer*>(commands.get())
                      ->getComputeCommandEncoder();
        compute->setComputePipelineState(stencil_pipeline);
        compute->setTexture(stencil_native, 0);
        compute->setBuffer(stencil_result, 0, 0);
        compute->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        commands.reset();
        const auto* stencil_values =
            static_cast<const u32*>(stencil_result->contents());
        std::cout << "stencil read: " << stencil_values[0] << " "
                  << stencil_values[1] << " " << stencil_values[2] << " "
                  << stencil_values[3] << "\n";
        check(stencil_values[0] == 0x5a && stencil_values[1] == 0x5a &&
                  stencil_values[2] == 0x5a && stencil_values[3] == 0x5a,
              "RRRR swizzle returns the raw stencil value in every component");
        stencil_result->release();
        stencil_pipeline->release();
        stencil_function->release();
        stencil_library->release();
        std::cout << "PASS: Z24S8 RRRR swizzle samples the stencil plane\n";

        {
            std::vector<u8> copy_memory(512, 0x55);
            for (u32 i = 0; i < 64; ++i)
                copy_memory[i] = static_cast<u8>(i * 3);
            metal::Renderer copy_renderer;
            auto& copy_cache = copy_renderer.getTextureCache();
            const auto address = reinterpret_cast<uptr>(copy_memory.data());
            const auto make_copy_desc = [](uptr ptr_) {
                return TextureDescriptor(ptr_, TextureType::_2D,
                    TextureFormat::RGBA8Unorm, true, 16, 4, 4, 1, 1, 1, 0, 0, 0);
            };
            commands.reset(copy_renderer.createCommandBuffer());
            auto* source = copy_cache.find(commands.get(), make_copy_desc(address), TextureUsage::Read);
            auto* destination = copy_cache.find(commands.get(), make_copy_desc(address + 256), TextureUsage::Read);
            copy_renderer.blitTexture(commands.get(), source, {1.0f, 1.0f, 0.0f},
                {2, 2, 1}, 0, 0, destination, {2.0f, 2.0f, 0.0f}, {2, 2, 1}, 0, 0, 1, 1, {});
            auto* native = static_cast<metal::TextureView*>(destination)->getTexture();
            auto* copy_result = copy_renderer.getDevice()->newBuffer(64, MTL::ResourceStorageModeShared);
            auto* copy_blit = static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder();
            copy_blit->copyFromTexture(native, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(4, 4, 1),
                                  copy_result, 0, 16, 64);
            commands.reset();
            const auto* bytes = static_cast<const u8*>(copy_result->contents());
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    for (u32 c = 0; c < 4; ++c) {
                        const auto expected = x >= 2 && y >= 2
                            ? copy_memory[((y - 1) * 4 + x - 1) * 4 + c] : u8(0x55);
                        check(bytes[(y * 4 + x) * 4 + c] == expected,
                              "Native 2D copy preserves RGBA and untouched neighboring pixels");
                    }
                }
            }
            copy_result->release();
            std::cout << "PASS: native subrectangle texture blit preserves alpha\n";
        }

        // Scaling and format conversion use the render fallback. Guest alpha
        // must be copied even when the display pipeline is already cached.
        for (const auto format : {TextureFormat::RGBA8Unorm,
                                  TextureFormat::RGBA16Unorm}) {
            std::vector<u8> memory(512, 0x55);
            const u8 scaled_rgba[] = {51, 102, 153, 204};
            std::copy(std::begin(scaled_rgba), std::end(scaled_rgba), memory.begin());
            metal::Renderer copy_renderer;
            auto& pipelines = copy_renderer.getBlitPipelineCache();
            const auto pixel_format = format == TextureFormat::RGBA8Unorm
                ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatRGBA16Unorm;
            const metal::BlitPipelineDescriptor display_desc{
                .pixel_format = pixel_format, .transparent = false};
            auto copy_desc = display_desc;
            copy_desc.copy_alpha = true;
            auto* display_pipeline = pipelines.find(display_desc);
            check(display_pipeline != nullptr, "Display blit pipeline compiles");
            check(pipelines.find(copy_desc) != display_pipeline,
                  "Guest alpha copy has a separate pipeline cache key");
            check(pipelines.find(display_desc) == display_pipeline,
                  "Display pipeline remains available after guest copy");
            const auto address = reinterpret_cast<uptr>(memory.data());
            commands.reset(copy_renderer.createCommandBuffer());
            auto& copy_cache = copy_renderer.getTextureCache();
            auto* source = copy_cache.find(commands.get(), TextureDescriptor(
                address, TextureType::_2D, TextureFormat::RGBA8Unorm,
                true, 4, 1, 1, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            const u32 bpp = format == TextureFormat::RGBA8Unorm ? 4 : 8;
            auto* destination = copy_cache.find(commands.get(), TextureDescriptor(
                address + 256, TextureType::_2D, format,
                true, 4 * bpp, 4, 4, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            copy_renderer.blitTexture(commands.get(), source, {0, 0, 0},
                {1, 1, 1}, 0, 0, destination, {1, 1, 0}, {2, 2, 1},
                0, 0, 1, 1, {});
            auto* native = static_cast<metal::TextureView*>(destination)->getTexture();
            auto* scaled_result = copy_renderer.getDevice()->newBuffer(
                16 * bpp, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder()
                ->copyFromTexture(native, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(4, 4, 1),
                                  scaled_result, 0, 4 * bpp, 16 * bpp);
            commands.reset();
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    for (u32 c = 0; c < 4; ++c) {
                        const bool copied = x >= 1 && x < 3 && y >= 1 && y < 3;
                        const u32 i = (y * 4 + x) * 4 + c;
                        const int actual = bpp == 4
                            ? static_cast<const u8*>(scaled_result->contents())[i]
                            : static_cast<const u16*>(scaled_result->contents())[i];
                        const int expected = copied ? scaled_rgba[c] * (bpp == 4 ? 1 : 257)
                                                    : (bpp == 4 ? 0x55 : 0x5555);
                        check(std::abs(actual - expected) <= (copied ? 1 : 0),
                              "Scaled/converting blit copies RGBA and preserves neighbors");
                    }
                }
            }
            scaled_result->release();
            std::cout << "PASS: scaled/converting guest blit preserves alpha\n";
        }

        // Integer formats scale through an integer pipeline, point sampled
        {
            std::vector<u8> memory(512, 0);
            const u8 texels[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8},
                                     {9, 10, 11, 12}, {13, 14, 15, 16}};
            std::memcpy(memory.data(), texels, sizeof(texels));
            metal::Renderer int_renderer;
            const auto address = reinterpret_cast<uptr>(memory.data());
            commands.reset(int_renderer.createCommandBuffer());
            auto& int_cache = int_renderer.getTextureCache();
            auto* source = int_cache.find(commands.get(), TextureDescriptor(
                address, TextureType::_2D, TextureFormat::RGBA8Uint,
                true, 8, 2, 2, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            auto* destination = int_cache.find(commands.get(), TextureDescriptor(
                address + 256, TextureType::_2D, TextureFormat::RGBA8Uint,
                true, 16, 4, 4, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            BlitOptions options;
            options.nearest = true;
            options.src_width = 2.0f;
            options.src_height = 2.0f;
            int_renderer.blitTexture(commands.get(), source, {0, 0, 0},
                {2, 2, 1}, 0, 0, destination, {0, 0, 0}, {4, 4, 1},
                0, 0, 1, 1, options);
            auto* native = static_cast<metal::TextureView*>(destination)->getTexture();
            auto* int_result = int_renderer.getDevice()->newBuffer(
                64, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder()
                ->copyFromTexture(native, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(4, 4, 1),
                                  int_result, 0, 16, 64);
            commands.reset();
            const auto* bytes = static_cast<const u8*>(int_result->contents());
            bool match = true;
            for (u32 y = 0; y < 4; ++y)
                for (u32 x = 0; x < 4; ++x)
                    for (u32 c = 0; c < 4; ++c)
                        match &= bytes[(y * 4 + x) * 4 + c] ==
                                 texels[(y / 2) * 2 + x / 2][c];
            check(match, "Scaled integer blit replicates texels exactly");
            int_result->release();
            std::cout << "PASS: integer scaled blit\n";
        }

        // A CPU page/DMA readback of part of a tiled target must not
        // overwrite neighboring uploads, mip allocations or tile padding.
        {
            std::vector<u8> tiled_memory(1024, 0x55);
            auto expected_memory = tiled_memory;
            metal::Renderer tiled_renderer;
            auto& tiled_cache = tiled_renderer.getTextureCache();
            const auto tiled_address =
                reinterpret_cast<uptr>(tiled_memory.data());
            const TextureDescriptor tiled_desc(tiled_address, TextureType::_2D,
                                               TextureFormat::RGBA8Unorm, false,
                                               0, 8, 8, 1, 1, 1, 0, 0, 0);
            commands.reset(tiled_renderer.createCommandBuffer());
            auto* tiled_target = tiled_cache.find(commands.get(), tiled_desc,
                                                  TextureUsage::Write);
            auto* tiled_pass =
                MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* tiled_attachment = tiled_pass->colorAttachments()->object(0);
            tiled_attachment->setTexture(
                static_cast<metal::Texture*>(tiled_target->getBase())
                    ->getTexture());
            tiled_attachment->setLoadAction(MTL::LoadActionClear);
            tiled_attachment->setStoreAction(MTL::StoreActionStore);
            tiled_attachment->setClearColor(MTL::ClearColor(1, 0, 1, 0));
            static_cast<metal::CommandBuffer*>(commands.get())
                ->getRenderCommandEncoder(tiled_pass);
            const auto read_part = [&](u32 begin, u32 length) {
                tiled_cache.flushMemory(
                    commands.get(),
                    ztd::Range<uptr>::fromSize(tiled_address + begin, length));
                for (u32 y = 0; y < 8; ++y) {
                    for (u32 x = 0; x < 32; ++x) {
                        const auto byte =
                            hydra::hw::tegra_x1::gpu::getBlockLinearOffset(
                                x, y, 0, 32, 8, 0, 0);
                        if (byte >= begin && byte < begin + length)
                            expected_memory[byte] = x % 2 == 0 ? 255 : 0;
                    }
                }
                check(tiled_memory == expected_memory,
                      "Partial tiled readback preserves every unrequested byte "
                      "and padding");
            };
            read_part(20, 37);
            // A second partial request must still read the GPU result.
            tiled_memory[400] = expected_memory[400] = 0x33;
            read_part(72, 19);
            read_part(0, tiled_desc.size);
            commands.reset();
            std::cout << "PASS: repeated partial tiled readback preserves CPU "
                         "neighbors and padding\n";
        }

        // UE4 reads back a 1x1 RGBA32F render target (sky light average
        // brightness) through a DMA copy of guest memory. The rendered value
        // must reach guest memory, otherwise the game reads zeros.
        {
            std::vector<u8> readback_memory(4096, 0);
            const auto readback_ptr =
                reinterpret_cast<uptr>(readback_memory.data());
            auto readback_desc = TextureDescriptor(
                readback_ptr, TextureType::_2D, TextureFormat::RGBA32Float,
                false, 0, 1, 1, 1, 1, 1, 0, 0, 0);
            commands.reset(range_renderer.createCommandBuffer());
            auto* readback_target = range_cache.find(commands.get(), readback_desc,
                                            TextureUsage::Write);
            auto* readback_pass =
                MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* readback_attachment = readback_pass->colorAttachments()->object(0);
            readback_attachment->setTexture(
                static_cast<metal::Texture*>(readback_target->getBase())->getTexture());
            readback_attachment->setLoadAction(MTL::LoadActionClear);
            readback_attachment->setStoreAction(MTL::StoreActionStore);
            readback_attachment->setClearColor(MTL::ClearColor(0.25, 0.5, 4.0, 1.0));
            static_cast<metal::CommandBuffer*>(commands.get())
                ->getRenderCommandEncoder(readback_pass);
            range_cache.flushMemory(
                commands.get(),
                ztd::Range<uptr>::fromSize(readback_ptr, 16));
            const auto* values =
                reinterpret_cast<const f32*>(readback_memory.data());
            check(values[0] == 0.25f && values[1] == 0.5f &&
                      values[2] == 4.0f && values[3] == 1.0f,
                  "GPU-rendered RGBA32F target is flushed to guest memory");
            commands.reset();
            std::cout << "PASS: rendered texture flushes to guest memory\n";
        }

        // Guest readback must replay aliases by GPU write time, rather than
        // the cache's insertion/hash order. The first storage is deliberately
        // newer so an unordered flush overwrites it with the older alias.
        {
            std::vector<u8> memory(4096, 0);
            const auto address = reinterpret_cast<uptr>(memory.data());
            metal::Renderer alias_renderer;
            auto& aliases = alias_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> cb(alias_renderer.createCommandBuffer());
            for (auto format : {TextureFormat::RGBA32Float, TextureFormat::RGBA8Unorm}) {
                const TextureDescriptor desc(address, TextureType::_2D, format,
                                             false, 0, 1, 1, 1, 1, 1, 0, 0, 0);
                aliases.find(cb.get(), desc, TextureUsage::Read);
            }
            auto& mem = const_cast<TextureMem&>(aliases.getMemory(0));
            TextureStorage* newest = nullptr;
            const auto now = TextureCacheClock::now();
            for (auto& [group_key, group] : mem.cache) {
                for (auto& [storage_key, storage] : group.cache) {
                    if (newest == nullptr)
                        newest = &storage;
                    storage.gpu_written_timestamp =
                        now + std::chrono::seconds(&storage == newest ? 2 : 1);
                    auto* alias_pass = MTL::RenderPassDescriptor::renderPassDescriptor();
                    auto* alias_attachment = alias_pass->colorAttachments()->object(0);
                    alias_attachment->setTexture(static_cast<metal::Texture*>(storage.base)->getTexture());
                    alias_attachment->setLoadAction(MTL::LoadActionClear);
                    alias_attachment->setStoreAction(MTL::StoreActionStore);
                    alias_attachment->setClearColor(MTL::ClearColor(0.25, 0.5, 0.75, 1));
                    static_cast<metal::CommandBuffer*>(cb.get())->getRenderCommandEncoder(alias_pass);
                }
            }
            mem.info.written_timestamp = now + std::chrono::seconds(2);
            aliases.flushMemory(cb.get(), ztd::Range<uptr>::fromSize(address, 4096));
            check(newest != nullptr, "Readback alias storage exists");
            if (newest->base->getDescriptor().format == TextureFormat::RGBA32Float) {
                f32 value;
                std::memcpy(&value, memory.data(), sizeof(value));
                check(value == 0.25f, "Newest float alias survives guest readback");
            } else {
                check(memory[0] == 64 && memory[1] == 128 && memory[2] == 191 && memory[3] == 255,
                      "Newest byte alias survives guest readback");
            }
            cb.reset();
            std::cout << "PASS: guest readback preserves the newest GPU alias\n";
        }

        // DMA sub rectangle uploads address block linear memory per byte.
        // Every byte offset must match the whole surface conversion.
        for (const auto [width, height, block_height_log2] :
             {std::tuple<u32, u32, u32>{4096, 64, 4}, {512, 40, 2},
              {192, 24, 0}, {64, 8, 3}}) {
            std::vector<u8> linear(static_cast<usize>(width) * height);
            for (usize i = 0; i < linear.size(); i++)
                linear[i] = static_cast<u8>(i * 7 + i / 251);
            const auto layout_size =
                static_cast<usize>(align(width, 64u)) *
                align(height, 8u << block_height_log2);
            std::vector<u8> block_linear(layout_size, 0);
            hw::tegra_x1::gpu::convertLinearToBlockLinear(
                width, width * height, width, height, 1, block_height_log2, 0,
                linear.data(), block_linear.data());
            bool match = true;
            for (u32 y = 0; y < height && match; y++)
                for (u32 x = 0; x < width; x++)
                    if (block_linear[hw::tegra_x1::gpu::getBlockLinearOffset(
                            x, y, 0, width, height, block_height_log2, 0)] !=
                        linear[static_cast<usize>(y) * width + x]) {
                        match = false;
                        break;
                    }
            check(match, "Block linear byte offsets match the surface layout");
        }
        std::cout << "PASS: block linear byte offsets for DMA sub rectangles\n";

        // 3D block linear surfaces store the GOBs of every slice of a block
        // consecutively. Crash 4's volumetric lightmap (1280x25x5, 4 slices
        // per block) decoded as garbage when only one slice per block was
        // visited.
        for (const auto [width, height, depth, block_height_log2,
                         block_depth_log2] :
             {std::tuple<u32, u32, u32, u32, u32>{320, 25, 5, 1, 2},
              {128, 32, 16, 2, 4}, {64, 8, 3, 0, 1}}) {
            const usize slice = static_cast<usize>(width) * height;
            std::vector<u8> linear(slice * depth);
            for (usize i = 0; i < linear.size(); i++)
                linear[i] = static_cast<u8>(i * 13 + i / 241);
            const usize layout_size =
                static_cast<usize>(align(width, 64u)) *
                align(height, 8u << block_height_log2) *
                align(depth, 1u << block_depth_log2);
            std::vector<u8> block_linear(layout_size, 0);
            hw::tegra_x1::gpu::convertLinearToBlockLinear(
                width, static_cast<u32>(slice), width, height, depth,
                block_height_log2, block_depth_log2, linear.data(),
                block_linear.data());
            std::vector<u8> decoded(linear.size(), 0);
            hw::tegra_x1::gpu::convertBlockLinearToLinear(
                width, width, static_cast<u32>(slice), height, depth,
                block_height_log2, block_depth_log2, block_linear.data(),
                decoded.data());
            check(decoded == linear, "3D block linear round trip");
            bool match = true;
            for (u32 z = 0; z < depth && match; z++)
                for (u32 y = 0; y < height && match; y++)
                    for (u32 x = 0; x < width; x++)
                        if (block_linear[hw::tegra_x1::gpu::getBlockLinearOffset(
                                x, y, z, width, height, block_height_log2,
                                block_depth_log2)] !=
                            linear[z * slice + static_cast<usize>(y) * width +
                                   x]) {
                            match = false;
                            break;
                        }
            check(match, "3D block linear layout matches the hardware order");
        }
        std::cout << "PASS: 3D block linear slices within a block\n";


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

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

#include <array>
#include <cmath>
#include <cstring>
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
void runTextureCoherenceFuzz();

void testGpuOwnedAliasReplay() {
    using hydra::hw::tegra_x1::gpu::getBlockLinearOffset;
    for (const bool mip_chain : {true, false}) {
        std::vector<u8> memory(0x20000, 0x5a), expected = memory;
        const auto address = reinterpret_cast<uptr>(memory.data());
        const TextureDescriptor destination(address, MTL::TextureType2D,
            mip_chain ? TextureFormat::R8Unorm : TextureFormat::RG32Float,
            false, 0, mip_chain ? 256 : 64, mip_chain ? 256 : 64,
            1, mip_chain ? 9 : 1, 1, mip_chain ? 2 : 0, 5, 0);
        const auto source_descriptor = [&](u32 level) {
            return TextureDescriptor(address + (mip_chain
                    ? destination.getLevelOffset(level) : level * 0x4000),
                MTL::TextureType2D,
                mip_chain ? TextureFormat::R8Unorm : TextureFormat::RGBA32Float,
                false, 0, mip_chain ? 256u >> level : 32u >> level,
                mip_chain ? 256u >> level : 32u >> level,
                1, 1, 1, 0, mip_chain ? 5 - level : 2 - level, 0);
        };
        metal::Renderer renderer;
        auto& cache = renderer.getTextureCache();
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        auto* target = cache.find(commands.get(), destination, TextureUsage::Read);
        for (u32 level = 0; level < 2; ++level)
            cache.find(commands.get(), source_descriptor(level), TextureUsage::Read);
        const auto clear = [&](ITextureView* view, const TextureDescriptor& desc,
                               u32 level, bool source) {
            const auto dims = desc.getLevelDimensions(level);
            auto* native = static_cast<metal::Texture*>(view->getBase())->getTexture();
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(native);
            attachment->setLevel(level);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(source ? MTL::ClearColor(0.75, 1, 1.25, 1.5)
                                              : MTL::ClearColor(0.25, 0.5, 0, 1));
            static_cast<metal::CommandBuffer*>(commands.get())->getRenderCommandEncoder(pass);
            std::array<u8, 16> texel{};
            const std::array<f32, 4> color = source
                ? std::array<f32, 4>{0.75f, 1, 1.25f, 1.5f}
                : std::array<f32, 4>{0.25f, 0.5f, 0, 1};
            std::memcpy(texel.data(), color.data(), sizeof(color));
            if (mip_chain) texel[0] = source ? 191 : 64;
            const auto bytes = getTextureFormatInfo(desc.format).bytes_per_block;
            const auto block = desc.getLevelBlockSizeLog2(level);
            const auto stride = align(static_cast<u32>(dims.width) * bytes,
                                      64u << static_cast<u32>(block.width));
            for (u32 y = 0; y < dims.height; ++y) {
                for (u32 x = 0; x < dims.width; ++x) {
                    const auto offset = desc.ptr - address + desc.getLevelOffset(level) +
                        getBlockLinearOffset(x * bytes, y, 0, stride,
                            static_cast<u32>(dims.height), static_cast<u32>(block.height), 0);
                    std::memcpy(expected.data() + offset, texel.data(), bytes);
                }
            }
        };
        cache.find(commands.get(), destination, TextureUsage::Write);
        for (u32 level = 0; level < destination.level_count; ++level)
            clear(target, destination, level, false);
        for (u32 level = 0; level < 2; ++level) {
            const auto source = source_descriptor(level);
            auto* view = cache.find(commands.get(), source, TextureUsage::Write);
            clear(view, source, 0, true);
            // Replaying the older destination must preserve newer aliases
            // even after those aliases have already reached guest memory.
            cache.flushMemory(commands.get(), source.getRange());
        }
        target = cache.find(commands.get(), destination, TextureUsage::Read);
        for (u32 level = 0; level < destination.level_count; ++level) {
            const auto dims = destination.getLevelDimensions(level);
            const auto bytes = getTextureFormatInfo(destination.format).bytes_per_block;
            const u32 row_bytes = static_cast<u32>(dims.width) * bytes;
            const u32 image_bytes = row_bytes * static_cast<u32>(dims.height);
            NS_STACK_SCOPED auto result = renderer.getDevice()->newBuffer(
                image_bytes, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder()
                ->copyFromTexture(static_cast<metal::Texture*>(target->getBase())->getTexture(),
                    0, level, MTL::Origin(0, 0, 0), dims, result, 0, row_bytes, image_bytes);
            check(renderer.synchronize(commands.get()), "GPU alias replay readback completes");
            const auto block = destination.getLevelBlockSizeLog2(level);
            const auto stride = align(row_bytes, 64u << static_cast<u32>(block.width));
            for (u32 y = 0; y < dims.height; ++y) {
                for (u32 x = 0; x < dims.width; ++x) {
                    const auto offset = destination.getLevelOffset(level) +
                        getBlockLinearOffset(x * bytes, y, 0, stride,
                            static_cast<u32>(dims.height), static_cast<u32>(block.height), 0);
                    check(std::memcmp(static_cast<const u8*>(result->contents()) +
                              y * row_bytes + x * bytes, expected.data() + offset, bytes) == 0,
                          "Alias replay preserves older GPU texels and the newest overlapping bytes");
                }
            }
        }
    }
    std::cout << "PASS: GPU-owned mip/layout and different-width aliases replay in byte order\n";
}

int main() {
    try {
        // Exercise the real cache and Metal texture views, including
        // overlapping guest memory and contained mip levels from the CTR crash
        // path.
        setenv("HYDRA_GPU_SYNC", "1", 1);
        setenv("HYDRA_TEXTURE_EVENTS", "off", 0);
        setenv("HYDRA_PERF_STATS", "1", 1);
        setenv("HYDRA_DISABLE_SHADER_CACHE", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        runTextureFormatTests();
        runTextureCoherenceFuzz();
        testGpuOwnedAliasReplay();
        std::vector<u8> guest_memory(65536, 0);
        metal::Renderer renderer;
        std::unique_ptr<ICommandBuffer> commands(
            renderer.createCommandBuffer());
        auto& cache = renderer.getTextureCache();
        const auto ptr = reinterpret_cast<uptr>(guest_memory.data());
        const auto descriptor = [ptr](TextureFormat format) {
            return TextureDescriptor(ptr, MTL::TextureType2D, format, false, 0,
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
            ptr + bc5.getLevelOffset(1), MTL::TextureType2D,
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
            const TextureDescriptor first(address, MTL::TextureType2D,
                TextureFormat::RGBA8Unorm, variant == 1, variant == 1 ? 256 : 0,
                32, 32, 1, 1, 1, 0, 0, 0);
            const TextureDescriptor second(address, MTL::TextureType2D,
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
            const TextureDescriptor cube(cube_ptr, MTL::TextureType2DArray,
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
                cube_ptr + 0x20000, MTL::TextureType2DArray,
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
            const TextureDescriptor full(cube_ptr, MTL::TextureType2DArray,
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
            const TextureDescriptor sky(cube_ptr, MTL::TextureType2DArray,
                                        TextureFormat::RGBA16Float, false, 0,
                                        128, 128, 1, 8, 6, 0, 4, 0, 0x2c000);
            auto* sky_view =
                cache.find(sky_commands.get(), sky, TextureUsage::Read);
            const TextureDescriptor padded(
                cube_ptr + 4 * 0x2c000 + sky.getLevelOffset(5),
                MTL::TextureType2D, TextureFormat::RGBA16Float, false, 0, 8, 4,
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
                alias_ptr, MTL::TextureType2DArray, TextureFormat::RGBA8Unorm,
                false, 0, 32, 32, 1, 3, 2, 0, 0, 0, 0x2000);
            const TextureDescriptor destination(
                alias_ptr + source.layer_size + source.getLevelOffset(1),
                MTL::TextureType2DArray, TextureFormat::RGBA8Unorm, false, 0,
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

        // Unreal streams a BC texture mip by mip through RGBA32 render targets
        // whose texel is one block, placed at the address of the mip. The BC
        // texture sampled afterwards must receive those blocks (this used to
        // leave it stale: random coloured blocks on the streamed materials).
        {
            std::vector<u8> stream_memory(0x20000, 0);
            const auto stream_ptr = reinterpret_cast<uptr>(stream_memory.data());
            metal::Renderer stream_renderer;
            auto& stream_cache = stream_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> stream_commands(
                stream_renderer.createCommandBuffer());
            const TextureDescriptor bc3(stream_ptr, MTL::TextureType2D,
                                        TextureFormat::BC3_RGBA, false, 0, 64,
                                        64, 1, 5, 1, 0, 0, 0);
            struct Mip {
                u32 level, blocks;
                MTL::ClearColor color;
            };
            const std::array<Mip, 2> mips{{{1, 8, MTL::ClearColor(1, 0.5, 0.25, 0.125)},
                                           {2, 4, MTL::ClearColor(2, 4, 8, 16)}}};
            stream_cache.find(stream_commands.get(), bc3, TextureUsage::Read);
            for (const auto& stream_mip : mips) {
                const TextureDescriptor target(
                    stream_ptr + bc3.getLevelOffset(stream_mip.level),
                    MTL::TextureType2D, TextureFormat::RGBA32Float, false, 0,
                    stream_mip.blocks, stream_mip.blocks, 1, 1, 1, 0, 0, 0);
                auto* target_view = stream_cache.find(
                    stream_commands.get(), target, TextureUsage::Read);
                stream_cache.find(stream_commands.get(), target,
                                  TextureUsage::Write);
                auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
                auto* attachment = pass->colorAttachments()->object(0);
                attachment->setTexture(
                    static_cast<metal::Texture*>(target_view->getBase())
                        ->getTexture());
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearColor(stream_mip.color);
                static_cast<metal::CommandBuffer*>(stream_commands.get())
                    ->getRenderCommandEncoder(pass);
            }
            auto* bc3_view = stream_cache.find(stream_commands.get(), bc3,
                                               TextureUsage::Read);
            auto* bc3_texture =
                static_cast<metal::Texture*>(bc3_view->getBase())->getTexture();
            NS_STACK_SCOPED auto readback =
                stream_renderer.getDevice()->newBuffer(
                    0x2000, MTL::ResourceStorageModeShared);
            auto* blit = static_cast<metal::CommandBuffer*>(stream_commands.get())
                             ->getBlitCommandEncoder();
            for (u32 i = 0; i < mips.size(); ++i) {
                const u32 blocks = mips[i].blocks;
                blit->copyFromTexture(
                    bc3_texture, 0, mips[i].level, MTL::Origin(0, 0, 0),
                    MTL::Size(blocks * 4, blocks * 4, 1), readback,
                    i * 0x1000, blocks * 16, blocks * blocks * 16);
            }
            stream_commands.reset();
            for (u32 i = 0; i < mips.size(); ++i) {
                const f32 expected[4] = {static_cast<f32>(mips[i].color.red),
                                         static_cast<f32>(mips[i].color.green),
                                         static_cast<f32>(mips[i].color.blue),
                                         static_cast<f32>(mips[i].color.alpha)};
                const auto* bytes =
                    static_cast<const u8*>(readback->contents()) + i * 0x1000;
                bool all_match = true;
                for (u32 block = 0; block < mips[i].blocks * mips[i].blocks; ++block)
                    all_match &= std::memcmp(bytes + block * 16, expected, 16) == 0;
                check(all_match,
                      "BC3 mip written through RGBA32 blocks reaches the BC texture");
            }
            std::cout << "PASS: BC blocks written through RGBA32 render targets\n";
        }

        // Unreal writes the small levels of a mip chain as textures of their
        // own, one GOB (64 bytes) wide: a 32x32 level as 64x32, 16x16 as 64x16
        // and so on, at the address of that level inside the chain (the 256x256
        // and 512x512 R8 chains of the Crash Bandicoot 4 log). The chain has
        // GPU data of its own, so it cannot be rebuilt from guest memory: the
        // levels must be copied from the textures that were written, in both
        // directions, without touching the padding columns.
        {
            std::vector<u8> tail_memory(0x20000, 0);
            const auto tail_ptr = reinterpret_cast<uptr>(tail_memory.data());
            metal::Renderer tail_renderer;
            auto& tail_cache = tail_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> tail_commands(
                tail_renderer.createCommandBuffer());
            const TextureDescriptor chain(tail_ptr, MTL::TextureType2D,
                                          TextureFormat::R8Unorm, false, 0, 256,
                                          256, 1, 9, 1, 2, 5, 0);
            check(chain.size == 0x16400 && chain.getLevelOffset(3) == 0x15000 &&
                      chain.getLevelSize(3) == 0x800 &&
                      chain.getLevelOffset(8) == 0x16200,
                  "The R8 chain has the layout of the log");
            const auto clear = [&](ITextureView* view, u32 level, f64 value) {
                auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
                auto* attachment = pass->colorAttachments()->object(0);
                attachment->setTexture(
                    static_cast<metal::Texture*>(view->getBase())->getTexture());
                attachment->setLevel(level);
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearColor(MTL::ClearColor(value, 0, 0, 1));
                static_cast<metal::CommandBuffer*>(tail_commands.get())
                    ->getRenderCommandEncoder(pass);
            };
            // What each tail texture is cleared to, exact in R8Unorm
            const auto tailByte = [](u32 level) {
                return static_cast<u8>(40 + 20 * level);
            };
            struct Tail {
                u32 level, width, height, block_height_log2;
            };
            const std::array<Tail, 6> tails{{{3, 64, 32, 2}, {4, 64, 16, 1},
                                             {5, 64, 8, 0}, {6, 64, 4, 0},
                                             {7, 64, 2, 0}, {8, 64, 1, 0}}};
            auto* chain_view = tail_cache.find(tail_commands.get(), chain,
                                               TextureUsage::Read);
            tail_cache.find(tail_commands.get(), chain, TextureUsage::Write);
            clear(chain_view, 0, 0.5);
            for (const auto& level : tails) {
                const TextureDescriptor tail(
                    tail_ptr + chain.getLevelOffset(level.level),
                    MTL::TextureType2D, TextureFormat::R8Unorm, false, 0,
                    level.width, level.height, 1, 1, 1, 0,
                    level.block_height_log2, 0);
                check(tail.size == chain.getLevelSize(level.level),
                      "A tail texture covers exactly its level of the chain");
                auto* tail_view = tail_cache.find(tail_commands.get(), tail,
                                                  TextureUsage::Read);
                tail_cache.find(tail_commands.get(), tail, TextureUsage::Write);
                clear(tail_view, 0, tailByte(level.level) / 255.0);
            }
            chain_view = tail_cache.find(tail_commands.get(), chain,
                                         TextureUsage::Read);
            auto* chain_texture =
                static_cast<metal::Texture*>(chain_view->getBase())->getTexture();
            NS_STACK_SCOPED auto readback = tail_renderer.getDevice()->newBuffer(
                0x4000, MTL::ResourceStorageModeShared);
            auto* blit = static_cast<metal::CommandBuffer*>(tail_commands.get())
                             ->getBlitCommandEncoder();
            for (u32 i = 0; i < tails.size(); ++i) {
                const u32 side = 32u >> i;
                blit->copyFromTexture(chain_texture, 0, tails[i].level,
                                      MTL::Origin(0, 0, 0),
                                      MTL::Size(side, side, 1), readback,
                                      i * 0x800, side, side * side);
            }
            // The other direction: the chain is written after the tail
            // texture, which is then sampled
            clear(chain_view, 3, 230.0 / 255.0);
            tail_cache.find(tail_commands.get(), chain, TextureUsage::Write);
            const TextureDescriptor tail3(
                tail_ptr + chain.getLevelOffset(3), MTL::TextureType2D,
                TextureFormat::R8Unorm, false, 0, 64, 32, 1, 1, 1, 0, 2, 0);
            auto* tail3_view = tail_cache.find(tail_commands.get(), tail3,
                                               TextureUsage::Read);
            auto* tail3_texture =
                static_cast<metal::Texture*>(tail3_view->getBase())->getTexture();
            blit = static_cast<metal::CommandBuffer*>(tail_commands.get())
                       ->getBlitCommandEncoder();
            blit->copyFromTexture(tail3_texture, 0, 0, MTL::Origin(0, 0, 0),
                                  MTL::Size(64, 32, 1), readback, 0x3000, 64,
                                  64 * 32);
            tail_commands.reset();
            const auto* bytes = static_cast<const u8*>(readback->contents());
            for (u32 i = 0; i < tails.size(); ++i) {
                const u32 side = 32u >> i;
                const u8 expected = tailByte(tails[i].level);
                bool all_match = true;
                u32 first_wrong = 0;
                for (u32 y = 0; y < side; ++y)
                    for (u32 x = 0; x < side; ++x)
                        if (bytes[i * 0x800 + y * side + x] != expected) {
                            if (all_match)
                                first_wrong = bytes[i * 0x800 + y * side + x];
                            all_match = false;
                        }
                check(all_match,
                      fmt::format("Mip tail level {} written GOB wide reaches its level of "
                                  "the chain (expected {}, found {})",
                                  tails[i].level, expected, first_wrong)
                          .c_str());
            }
            bool chain_into_tail = true;
            bool padding_kept = true;
            for (u32 y = 0; y < 32; ++y) {
                for (u32 x = 0; x < 64; ++x) {
                    const u8 value = bytes[0x3000 + y * 64 + x];
                    if (x < 32)
                        chain_into_tail &= value == 230;
                    else
                        padding_kept &= value == tailByte(3); // the tail's own
                }
            }
            check(chain_into_tail, "A level of the chain reaches the tail texture of that level");
            check(padding_kept, "The padding columns of the tail texture are left alone");
            std::cout << "PASS: mip tail textures written GOB wide synchronize with their chain\n";
        }

        // A bridge merges initially separate intervals after a GPU write.
        // Disjoint sampled textures must remain untouched, while an overlapping
        // padded alias must receive the rendered pixels through
        // synchronization.
        metal::Renderer range_renderer;
        auto& range_cache = range_renderer.getTextureCache();
        commands.reset(range_renderer.createCommandBuffer());
        auto target_desc =
            TextureDescriptor(ptr, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                              false, 0, 16, 16, 1, 1, 1, 0, 0, 0);
        auto alias_desc =
            TextureDescriptor(ptr, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                              false, 0, 8, 16, 1, 1, 1, 0, 0, 0);
        auto separate_desc = TextureDescriptor(ptr + 4096, MTL::TextureType2D,
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
            TextureDescriptor(ptr, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
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
            const TextureDescriptor raw_desc(address, MTL::TextureTypeTextureBuffer,
                TextureFormat::R32Uint, true, 8192, 2048, 1, 1, 1, 1, 0, 0, 0);
            const u32 height = linear_surface ? 3 : 16;
            const TextureDescriptor surface_desc(address + 512, MTL::TextureType2D,
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
                return hydra::hw::tegra_x1::gpu::getBlockLinearOffset(x, y, 0, 128, height, static_cast<u32>(blocks.height), static_cast<u32>(blocks.depth));
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
        auto buffer_full = TextureDescriptor(buffer_ptr, MTL::TextureTypeTextureBuffer,
                                             TextureFormat::R32Uint, true, 256,
                                             64, 1, 1, 1, 1, 0, 0, 0);
        auto buffer_sub = TextureDescriptor(
            buffer_ptr + 64, MTL::TextureTypeTextureBuffer, TextureFormat::R32Uint,
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
            buffer_ptr + 80, MTL::TextureTypeTextureBuffer, TextureFormat::R32Uint,
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
            buffer_ptr + 82, MTL::TextureTypeTextureBuffer, TextureFormat::R16Uint,
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
            buffer_ptr + 80, MTL::TextureTypeTextureBuffer, TextureFormat::R32Uint,
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
            TextureDescriptor(ptr + 16384, MTL::TextureType2D, sampled_format,
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
            TextureViewDescriptor(MTL::TextureType2D, sampled_format,
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
                MTL::TextureType2D, stencil_format, ztd::Range<u32>(0, 1),
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
                return TextureDescriptor(ptr_, MTL::TextureType2D,
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
                address, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                true, 4, 1, 1, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            const u32 bpp = format == TextureFormat::RGBA8Unorm ? 4 : 8;
            auto* destination = copy_cache.find(commands.get(), TextureDescriptor(
                address + 256, MTL::TextureType2D, format,
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
                address, MTL::TextureType2D, TextureFormat::RGBA8Uint,
                true, 8, 2, 2, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            auto* destination = int_cache.find(commands.get(), TextureDescriptor(
                address + 256, MTL::TextureType2D, TextureFormat::RGBA8Uint,
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
            const TextureDescriptor tiled_desc(tiled_address, MTL::TextureType2D,
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
                readback_ptr, MTL::TextureType2D, TextureFormat::RGBA32Float,
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

        // Packed depth/stencil targets use separate Metal planes, but guest
        // memory expects one Z24S8 word per pixel.
        {
            std::vector<u8> memory(1024, 0);
            const auto address = reinterpret_cast<uptr>(memory.data());
            const TextureDescriptor desc(
                address, MTL::TextureType2D, TextureFormat::Z24Unorm_S8Uint,
                false, 0, 4, 4, 1, 1, 1, 0, 0, 0);
            metal::Renderer depth_renderer;
            auto& depth_cache = depth_renderer.getTextureCache();
            auto cb = std::unique_ptr<ICommandBuffer>(
                depth_renderer.createCommandBuffer());
            auto* depth_target_view =
                depth_cache.find(cb.get(), desc, TextureUsage::Write);
            auto* packed_depth_native = static_cast<metal::Texture*>(
                                            depth_target_view->getBase())
                                            ->getTexture();
            auto* packed_depth_pass =
                MTL::RenderPassDescriptor::renderPassDescriptor();
            packed_depth_pass->depthAttachment()->setTexture(
                packed_depth_native);
            packed_depth_pass->depthAttachment()->setLoadAction(
                MTL::LoadActionClear);
            packed_depth_pass->depthAttachment()->setStoreAction(
                MTL::StoreActionStore);
            packed_depth_pass->depthAttachment()->setClearDepth(0.5);
            packed_depth_pass->stencilAttachment()->setTexture(
                packed_depth_native);
            packed_depth_pass->stencilAttachment()->setLoadAction(
                MTL::LoadActionClear);
            packed_depth_pass->stencilAttachment()->setStoreAction(
                MTL::StoreActionStore);
            packed_depth_pass->stencilAttachment()->setClearStencil(0x5a);
            static_cast<metal::CommandBuffer*>(cb.get())
                ->getRenderCommandEncoder(packed_depth_pass);
            depth_cache.flushMemory(cb.get(), desc.getRange());
            bool match = true;
            for (u32 y = 0; y < 4 && match; y++) {
                for (u32 x = 0; x < 4; x++) {
                    const usize offset =
                        hydra::hw::tegra_x1::gpu::getBlockLinearOffset(
                            x * 4, y, 0, 64, 4, 0, 0);
                    u32 word;
                    std::memcpy(&word, memory.data() + offset, sizeof(word));
                    const u32 depth = word & 0xffffffu;
                    match &= (depth == 0x7fffffu || depth == 0x800000u) &&
                             word >> 24 == 0x5a;
                }
            }
            check(match, "Depth/stencil clear flushes in packed guest layout");
            cb.reset();
            std::cout << "PASS: packed depth/stencil target flush\n";
        }

        // Aliases with different texel byte widths must preserve guest bytes,
        // including the BGRA8 -> RG32Float pair seen in Crash 4.
        for (const bool reverse : {false, true}) {
            std::vector<u8> memory(4096, 0);
            const auto address = reinterpret_cast<uptr>(memory.data());
            metal::Renderer alias_renderer;
            auto& aliases = alias_renderer.getTextureCache();
            std::unique_ptr<ICommandBuffer> cb(alias_renderer.createCommandBuffer());
            const auto make = [address](bool wide) {
                return TextureDescriptor(address, MTL::TextureType2D,
                    wide ? TextureFormat::RG32Float : TextureFormat::BGRA8Unorm,
                    true, 64, wide ? 8 : 16, 4, 1, 1, 1, 0, 0, 0);
            };
            const auto dst_desc = make(!reverse);
            aliases.find(cb.get(), dst_desc, TextureUsage::Read);
            const auto src_desc = make(reverse);
            auto* src = aliases.find(cb.get(), src_desc, TextureUsage::Write);
            auto* width_pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* width_attachment = width_pass->colorAttachments()->object(0);
            auto* native_src = static_cast<metal::Texture*>(src->getBase())->getTexture();
            width_attachment->setTexture(native_src);
            width_attachment->setLoadAction(MTL::LoadActionClear);
            width_attachment->setStoreAction(MTL::StoreActionStore);
            width_attachment->setClearColor(MTL::ClearColor(0.25, 0.5, 0.75, 1));
            auto* width_commands = static_cast<metal::CommandBuffer*>(cb.get());
            width_commands->getRenderCommandEncoder(width_pass);
            NS_STACK_SCOPED auto expected = alias_renderer.getDevice()->newBuffer(
                256, MTL::ResourceStorageModeShared);
            width_commands->getBlitCommandEncoder()->copyFromTexture(native_src, 0, 0,
                MTL::Origin(0, 0, 0), MTL::Size(src_desc.width, 4, 1),
                expected, 0, 64, 256);
            auto* dst = aliases.find(cb.get(), dst_desc, TextureUsage::Read);
            NS_STACK_SCOPED auto width_result = alias_renderer.getDevice()->newBuffer(
                256, MTL::ResourceStorageModeShared);
            width_commands->getBlitCommandEncoder()->copyFromTexture(
                static_cast<metal::Texture*>(dst->getBase())->getTexture(), 0, 0,
                MTL::Origin(0, 0, 0), MTL::Size(dst_desc.width, 4, 1),
                width_result, 0, 64, 256);
            check(alias_renderer.synchronize(cb.get()), "Alias readback completed");
            check(std::memcmp(expected->contents(), width_result->contents(), 256) == 0,
                  "Different-width texture aliases preserve all guest bytes");
        }
        std::cout << "PASS: BGRA8/RG32Float aliases in both directions\n";

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
                const TextureDescriptor desc(address, MTL::TextureType2D, format,
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

        // GPU-written 3D textures must read back every slice and retile the
        // packed Metal data into the guest block-linear layout.
        {
            std::vector<u8> memory(4096, 0);
            const auto address = reinterpret_cast<uptr>(memory.data());
            const TextureDescriptor desc(
                address, MTL::TextureType3D, TextureFormat::RGBA8Unorm, false, 0,
                4, 4, 3, 1, 1, 0, 0, 2);
            metal::Renderer readback_renderer;
            auto& readback_cache = readback_renderer.getTextureCache();
            auto cb = std::unique_ptr<ICommandBuffer>(
                readback_renderer.createCommandBuffer());
            auto* volume_target =
                readback_cache.find(cb.get(), desc, TextureUsage::Write);
            std::vector<u8> linear(4 * 4 * 4 * 3);
            for (usize i = 0; i < linear.size(); i++)
                linear[i] = static_cast<u8>(i * 17 + i / 13);
            auto* upload = readback_renderer.getDevice()->newBuffer(
                linear.data(), linear.size(), MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(cb.get())
                ->getBlitCommandEncoder()
                ->copyFromBuffer(
                    upload, 0, 16, 64, MTL::Size(4, 4, 3),
                    static_cast<metal::Texture*>(volume_target->getBase())
                        ->getTexture(),
                    0, 0, MTL::Origin(0, 0, 0));
            upload->release();
            readback_cache.flushMemory(cb.get(), desc.getRange());
            bool match = true;
            for (u32 z = 0; z < 3 && match; z++) {
                for (u32 y = 0; y < 4 && match; y++) {
                    for (u32 x = 0; x < 16; x++) {
                        const usize guest =
                            hydra::hw::tegra_x1::gpu::getBlockLinearOffset(
                                x, y, z, 64, 4, 0, 2);
                        const usize packed =
                            static_cast<usize>(z) * 64 + y * 16 + x;
                        if (memory[guest] != linear[packed]) {
                            match = false;
                            break;
                        }
                    }
                }
            }
            check(match, "3D GPU readback restores every guest slice");
            cb.reset();
            std::cout << "PASS: 3D texture readback and block-linear retile\n";
        }


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

        // A render target is only partly written back when the guest touches
        // part of it. What the GPU holds in memory the guest reused since is
        // stale, so a later write back must not put it over the guest's data
        {
            std::vector<u8> reuse_memory(0x8000, 0);
            for (usize i = 0; i < reuse_memory.size(); ++i)
                reuse_memory[i] = static_cast<u8>(i * 7 + 1);
            const std::vector<u8> original = reuse_memory;
            const auto reuse_base = reinterpret_cast<uptr>(reuse_memory.data());
            std::unique_ptr<ICommandBuffer> target_commands(
                renderer.createCommandBuffer());
            const TextureDescriptor partial_target(reuse_base, MTL::TextureType2D,
                                           TextureFormat::RGBA8Unorm, false, 0,
                                           64, 64, 1, 1, 1, 0, 0, 0);
            cache.find(target_commands.get(), partial_target, TextureUsage::Write);
            // The guest reuses the start of the memory
            std::memset(reuse_memory.data(), 0xcd, 0x1000);
            cache.invalidateMemory(ztd::Range<uptr>::fromSize(reuse_base, 0x1000));
            cache.flushMemory(target_commands.get(),
                              ztd::Range<uptr>::fromSize(reuse_base, 0x4000));
            bool reused_kept = true;
            for (usize i = 0; i < 0x1000; ++i)
                reused_kept &= reuse_memory[i] == 0xcd;
            check(reused_kept,
                  "A write back keeps memory the guest reused since");
            bool rest_written = true;
            for (usize i = 0x1000; i < 0x4000; ++i)
                rest_written &= reuse_memory[i] == original[i];
            check(rest_written,
                  "A write back still writes the untouched rest of the target");
            target_commands.reset();
        }

        // A render target the guest overwrote since it was rendered to holds
        // stale data: a texture that aliases the same memory must read what
        // the guest wrote, not a copy of the stale render target
        {
            std::vector<u8> stale_memory(0x8000, 0);
            for (usize i = 0; i < stale_memory.size(); ++i)
                stale_memory[i] = static_cast<u8>(i * 7 + 1);
            const auto stale_base =
                reinterpret_cast<uptr>(stale_memory.data());
            std::unique_ptr<ICommandBuffer> stale_commands(
                renderer.createCommandBuffer());
            const TextureDescriptor stale_target(
                stale_base, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                false, 0, 64, 64, 1, 1, 1, 0, 0, 0);
            cache.find(stale_commands.get(), stale_target,
                       TextureUsage::Write);
            // The guest reuses the memory for a texture
            std::memset(stale_memory.data(), 0x5a, 0x4000);
            cache.invalidateMemory(
                ztd::Range<uptr>::fromSize(stale_base, 0x4000));
            const TextureDescriptor stale_alias(
                stale_base, MTL::TextureType2D, TextureFormat::BGRA8Unorm,
                false, 0, 64, 64, 1, 1, 1, 0, 0, 0);
            auto* stale_view =
                cache.find(stale_commands.get(), stale_alias, TextureUsage::Read);
            auto* stale_readback = renderer.getDevice()->newBuffer(
                4, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(stale_commands.get())
                ->getBlitCommandEncoder()
                ->copyFromTexture(
                    static_cast<metal::Texture*>(stale_view->getBase())
                        ->getTexture(),
                    0, 0, MTL::Origin(0, 0, 0), MTL::Size(1, 1, 1),
                    stale_readback, 0, 4, 4);
            stale_commands.reset();
            const auto* stale_texel =
                static_cast<const u8*>(stale_readback->contents());
            const bool guest_data = stale_texel[0] == 0x5a &&
                                    stale_texel[1] == 0x5a &&
                                    stale_texel[2] == 0x5a &&
                                    stale_texel[3] == 0x5a;
            stale_readback->release();
            check(guest_data,
                  "An aliasing texture reads the guest's data, not a stale "
                  "render target");
        }

        // A render target that was written back and then overwritten by the
        // guest (streaming reuses the memory of dead render targets) holds
        // stale data too. Textures created over the memory afterwards must
        // neither copy it nor write it back over the guest's data
        {
            std::vector<u8> reused_memory(0x8000, 0);
            const auto reused_base = reinterpret_cast<uptr>(reused_memory.data());
            std::unique_ptr<ICommandBuffer> reused_commands(
                renderer.createCommandBuffer());
            // A small target in the middle of what becomes a larger texture
            const TextureDescriptor small_target(
                reused_base + 0x400, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                false, 0, 16, 8, 1, 1, 1, 0, 0, 0);
            cache.find(reused_commands.get(), small_target, TextureUsage::Write);
            cache.flushMemory(reused_commands.get(), small_target.getRange());
            // The guest streams a texture over the memory
            std::memset(reused_memory.data(), 0x5a, 0x2000);
            cache.invalidateMemory(ztd::Range<uptr>::fromSize(reused_base, 0x2000));
            const TextureDescriptor streamed(
                reused_base, MTL::TextureType2D, TextureFormat::BC5_RGUnorm, false,
                0, 64, 64, 1, 1, 1, 0, 0, 0);
            cache.find(reused_commands.get(), streamed, TextureUsage::Read);
            reused_commands.reset();
            bool streamed_intact = true;
            for (usize i = 0; i < 0x2000; ++i)
                streamed_intact &= reused_memory[i] == 0x5a;
            check(streamed_intact,
                  "A written back render target the guest overwrote does not "
                  "overwrite the guest's data again");
            // And an alias of the same size reads the guest's bytes
            std::vector<u8> same_memory(0x8000, 0);
            const auto same_base = reinterpret_cast<uptr>(same_memory.data());
            std::unique_ptr<ICommandBuffer> same_commands(
                renderer.createCommandBuffer());
            const TextureDescriptor same_target(
                same_base, MTL::TextureType2D, TextureFormat::RGBA8Unorm, false, 0,
                64, 64, 1, 1, 1, 0, 0, 0);
            cache.find(same_commands.get(), same_target, TextureUsage::Write);
            cache.flushMemory(same_commands.get(), same_target.getRange());
            std::memset(same_memory.data(), 0x5a, 0x4000);
            cache.invalidateMemory(ztd::Range<uptr>::fromSize(same_base, 0x4000));
            const TextureDescriptor same_alias(
                same_base, MTL::TextureType2D, TextureFormat::BGRA8Unorm, false, 0,
                64, 64, 1, 1, 1, 0, 0, 0);
            auto* same_view =
                cache.find(same_commands.get(), same_alias, TextureUsage::Read);
            auto* same_readback = renderer.getDevice()->newBuffer(
                4, MTL::ResourceStorageModeShared);
            static_cast<metal::CommandBuffer*>(same_commands.get())
                ->getBlitCommandEncoder()
                ->copyFromTexture(
                    static_cast<metal::Texture*>(same_view->getBase())->getTexture(),
                    0, 0, MTL::Origin(0, 0, 0), MTL::Size(1, 1, 1), same_readback,
                    0, 4, 4);
            same_commands.reset();
            const auto* same_texel =
                static_cast<const u8*>(same_readback->contents());
            const bool same_guest = same_texel[0] == 0x5a && same_texel[1] == 0x5a &&
                                    same_texel[2] == 0x5a && same_texel[3] == 0x5a;
            same_readback->release();
            check(same_guest,
                  "An alias reads the guest's data, not a render target that "
                  "was written back before the guest overwrote it");
        }

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

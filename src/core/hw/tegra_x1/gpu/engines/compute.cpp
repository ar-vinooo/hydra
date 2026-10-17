#include "core/hw/tegra_x1/gpu/engines/compute.hpp"

#include <fstream>
#include <set>

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/engines/storage_buffer.hpp"
#include "core/hw/tegra_x1/gpu/frame_tracer.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

namespace {

u32 getImageHandle(u32 handle) { return extractBits(handle, 0, 20); }
u32 getSamplerHandle(u32 handle) { return extractBits(handle, 20, 12); }

} // namespace

DEFINE_METHOD_TABLE(Compute, INLINE_ENGINE_TABLE, 0xaf, 1, launch, u32)

ztd::Range<uptr> Compute::getConstBuffer(const ComputeQmd& qmd,
                                         u32 index) const {
    if (index >= COMPUTE_CONST_BUFFER_COUNT ||
        (qmd.constBufferEnableMask() & (1u << index)) == 0)
        return {};
    const auto gpu_addr = qmd.constBufferAddress(index);
    const auto size = qmd.constBufferSize(index);
    if (gpu_addr == 0x0 || size == 0)
        return {};
    return ztd::Range<uptr>::fromSize(tls_crnt_gmmu->unmapAddr(gpu_addr),
                                      size);
}

std::optional<u32> Compute::readTextureHandle(const ComputeQmd& qmd,
                                              u32 binding_index) const {
    if (renderer::isConstantTextureIndex(binding_index))
        return renderer::getConstantTextureHandle(binding_index);

    u32 slot = regs.tex_cb_index;
    u32 word = binding_index;
    u32 sampler_word = renderer::TRACKED_TEXTURE_NO_SAMPLER;
    if (renderer::isTrackedTextureIndex(binding_index)) {
        slot = renderer::getTrackedTextureSlot(binding_index);
        word = renderer::getTrackedTextureWord(binding_index);
        sampler_word = renderer::getTrackedSamplerWord(binding_index);
    }

    const auto range = getConstBuffer(qmd, slot);
    const auto read_word = [&](u32 index) -> std::optional<u32> {
        if (range.getBegin() == 0x0 ||
            (static_cast<u64>(index) + 1) * sizeof(u32) > range.getSize())
            return std::nullopt;
        return reinterpret_cast<const u32*>(range.getBegin())[index];
    };

    auto handle = read_word(word);
    if (handle && sampler_word != renderer::TRACKED_TEXTURE_NO_SAMPLER) {
        const auto sampler = read_word(sampler_word);
        if (!sampler)
            return std::nullopt;
        *handle |= *sampler;
    }
    return handle;
}

void Compute::launch(const u32 index, const u32 arg) {
    (void)index;
    (void)arg;

    auto& renderer = gpu.getRenderer();
    auto& three_d = gpu.get3DEngine();

    // Same order as draws: the buffer cache, then the texture cache
    std::scoped_lock caches_lock(renderer.getBufferCache().getMutex(),
                                 renderer.getTextureCache().getMutex());

    // Flush tracked pages
    tls_crnt_gmmu->getMmu()->flushTrackedPages();

    const auto qmd_addr = static_cast<u64>(regs.launch_desc_loc) << 8;
    ComputeQmd qmd;
    std::memcpy(&qmd,
                reinterpret_cast<const void*>(
                    tls_crnt_gmmu->unmapAddr(qmd_addr)),
                sizeof(qmd));

    const auto code_base =
        (static_cast<u64>(regs.code_addr_hi) << 32) | regs.code_addr_lo;
    const auto program_addr = code_base + qmd.programStart();

    // DEBUG: dump compute programs
    if (const char* dir = std::getenv("HYDRA_DUMP_COMPUTE")) {
        static std::set<u64> dumped;
        if (dumped.insert(program_addr).second) {
            std::ofstream out(fmt::format("{}/cs_{:x}.bin", dir, program_addr),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(
                          tls_crnt_gmmu->unmapAddr(program_addr)),
                      0x4000);
            std::ofstream qmd_out(
                fmt::format("{}/cs_{:x}.qmd", dir, program_addr),
                std::ios::binary);
            qmd_out.write(reinterpret_cast<const char*>(&qmd), sizeof(qmd));
        }
    }

    // Pools
    const auto tic_addr =
        (static_cast<u64>(regs.tic_addr_hi) << 32) | regs.tic_addr_lo;
    const auto tsc_addr =
        (static_cast<u64>(regs.tsc_addr_hi) << 32) | regs.tsc_addr_lo;
    const auto* tic_pool =
        tic_addr != 0x0 ? reinterpret_cast<const TextureImageControl*>(
                              tls_crnt_gmmu->unmapAddr(tic_addr))
                        : nullptr;
    const auto* tsc_pool =
        tsc_addr != 0x0 ? reinterpret_cast<const TextureSamplerControl*>(
                              tls_crnt_gmmu->unmapAddr(tsc_addr))
                        : nullptr;
    const auto get_tic =
        [&](u32 binding_index) -> std::optional<TextureImageControl> {
        if (tic_pool == nullptr)
            return std::nullopt;
        const auto handle = readTextureHandle(qmd, binding_index);
        if (!handle)
            return std::nullopt;
        const auto image_handle = getImageHandle(*handle);
        if (image_handle > regs.tic_limit)
            return std::nullopt;
        return tic_pool[image_handle];
    };

    // Shader
    renderer::GuestShaderDescriptor descriptor{
        .stage = ShaderStage::Compute,
        .code_ptr = tls_crnt_gmmu->unmapAddr(program_addr),
        .gpu_addr = program_addr,
    };
    descriptor.state.program_offset = qmd.programStart();
    descriptor.state.shared_memory_size = qmd.sharedAlloc();
    descriptor.read_const_word = [&](u32 slot, u32 word) -> std::optional<u32> {
        const auto range = getConstBuffer(qmd, slot);
        if (range.getBegin() == 0 || word >= range.getSize() / sizeof(u32))
            return std::nullopt;
        u32 value;
        std::memcpy(&value,
                    reinterpret_cast<const void*>(range.getBegin() +
                                                  word * sizeof(u32)),
                    sizeof(value));
        return value;
    };
    auto shader = renderer.getShaderCache().find(descriptor);
    if (shader == nullptr)
        return;

    // Specialize for buffer textures and image formats
    {
        const auto& resource_mapping = shader->getDescriptor().resource_mapping;
        for (const auto& [binding_index, renderer_index] :
             resource_mapping.textures) {
            const auto type_it =
                resource_mapping.texture_types.find(binding_index);
            const bool is_1d =
                type_it != resource_mapping.texture_types.end() &&
                type_it->second == renderer::TextureType::_1D;
            const auto handle = readTextureHandle(qmd, binding_index);
            const auto tic = get_tic(binding_index);
            if (!tic)
                continue;
            if (tic->hdr_version != TicHdrVersion::_1DBuffer &&
                !tic->normalized_coords)
                descriptor.state.unnormalized_textures.push_back(binding_index);
            const auto format =
                renderer::toSampledTextureFormat(tic->format_word, tic->is_srgb);
            if (handle && tsc_pool != nullptr && tic->hdr_version != TicHdrVersion::_1DBuffer &&
                getSamplerHandle(*handle) <= regs.tsc_limit)
                specializeShaderSampler(descriptor.state, binding_index,
                                        tsc_pool[getSamplerHandle(*handle)],
                                        format, getTicLodRange(*tic));
            if (is_1d && tic->hdr_version == TicHdrVersion::_1DBuffer)
                descriptor.state.buffer_texture_formats.emplace(binding_index,
                                                                format);
            else if (tic->hdr_version != TicHdrVersion::_1DBuffer &&
                     renderer::getIntegerTextureComponentType(format))
                descriptor.state.integer_texture_formats.emplace(
                    binding_index, format);
        }
        for (const auto& [binding_index, renderer_index] :
             resource_mapping.images) {
            const auto tic = get_tic(binding_index);
            if (tic)
                descriptor.state.image_formats.emplace(
                    binding_index, renderer::toTextureFormat(tic->format_word,
                                                             tic->is_srgb));
        }
        if (!descriptor.state.texture_lod_bias.empty() ||
            !descriptor.state.border_samplers.empty() ||
            !descriptor.state.unnormalized_textures.empty() ||
            !descriptor.state.buffer_texture_formats.empty() ||
            !descriptor.state.integer_texture_formats.empty() ||
            !descriptor.state.image_formats.empty())
            shader = renderer.getShaderCache().find(descriptor);
        if (shader == nullptr)
            return;
    }
    const auto& resource_mapping = shader->getDescriptor().resource_mapping;

    renderer::ComputeDispatch dispatch{};
    dispatch.shader = shader;
    dispatch.debug_program_addr = program_addr;
    dispatch.grid_size = uint3({qmd.gridDimX(), qmd.gridDimY(), qmd.gridDimZ()});
    dispatch.block_size =
        uint3({qmd.blockDimX(), qmd.blockDimY(), qmd.blockDimZ()});

    // Uniform buffers
    for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
        const auto host_index = resource_mapping.uniform_buffers[i];
        if (host_index == invalid<u32>())
            continue;

        const auto range = getConstBuffer(qmd, i);
        if (range.getBegin() == 0x0) {
            LOG_WARN(Engines, "Compute uniform buffer {} is not bound", i);
            continue;
        }
        dispatch.uniform_buffers[host_index] =
            renderer.getBufferCache().get(tls_crnt_command_buffer, range);
    }

    // Storage buffers
    for (const auto& storage : resource_mapping.storage_buffers) {
        const auto range = resolveStorageBuffer(
            getConstBuffer(qmd, storage.cbuf_slot), storage.cbuf_offset);
        if (!range) {
            ONCE(LOG_WARN(Engines,
                          "Storage buffer descriptor c{}[{:#x}] is not valid",
                          storage.cbuf_slot, storage.cbuf_offset));
            continue;
        }
        dispatch.storage_buffers.push_back(
            {storage.host_index, renderer.getGuestMemoryBuffer(*range)});
        if (storage.written) {
            renderer.getBufferCache().markGpuModified(*range);
            renderer.getBufferCache().invalidateMemory(*range);
        }
    }

    // Textures
    struct TracedTexture {
        u32 binding_index;
        TextureImageControl tic;
        renderer::ITextureView* view;
    };
    std::vector<TracedTexture> traced_textures;
    for (const auto& [binding_index, renderer_index] :
         resource_mapping.textures) {
        if (tsc_pool == nullptr)
            break;
        const auto handle = readTextureHandle(qmd, binding_index);
        const auto tic = get_tic(binding_index);
        if (!handle || !tic || getSamplerHandle(*handle) > regs.tsc_limit)
            continue;

        const auto type_it = resource_mapping.texture_types.find(binding_index);
        const auto texture = three_d.getTexture(
            *tic, type_it != resource_mapping.texture_types.end()
                      ? std::optional(type_it->second)
                      : std::nullopt);
        const auto sampler = three_d.getSampler(
            tsc_pool[getSamplerHandle(*handle)],
            texture != nullptr ? texture->getDescriptor().format
                               : renderer::TextureFormat::Invalid,
            getTicLodRange(*tic));
        if (texture != nullptr && sampler != nullptr) {
            dispatch.textures.push_back({renderer_index, texture, sampler});
            // DEBUG: record and dump the textures in the frame trace
            if (FrameTracer::instance().isActive())
                traced_textures.push_back({binding_index, *tic, texture});
        }
    }

    // Images
    for (const auto& [binding_index, renderer_index] :
         resource_mapping.images) {
        const auto tic = get_tic(binding_index);
        if (!tic)
            continue;

        const auto type_it = resource_mapping.image_types.find(binding_index);
        const auto texture = three_d.getTexture(
            *tic,
            type_it != resource_mapping.image_types.end()
                ? std::optional(type_it->second)
                : std::nullopt,
            renderer::TextureUsage::Write);
        if (texture != nullptr)
            dispatch.images.push_back({renderer_index, texture});
    }

    // DEBUG: describe images and skip programs listed in HYDRA_SKIP_COMPUTE
    {
        static std::set<u64> described;
        if (described.insert(program_addr).second) {
            std::string images;
            for (const auto& [binding_index, renderer_index] :
                 resource_mapping.images) {
                const auto tic = get_tic(binding_index);
                const auto type_it =
                    resource_mapping.image_types.find(binding_index);
                images += fmt::format(
                    " [{:#x}: type {}, format {}, hdr {}]", binding_index,
                    type_it != resource_mapping.image_types.end()
                        ? static_cast<u32>(type_it->second)
                        : 99u,
                    tic ? renderer::toTextureFormat(tic->format_word,
                                                    tic->is_srgb)
                        : renderer::TextureFormat::Invalid,
                    tic ? static_cast<u32>(tic->hdr_version) : 99u);
            }
            LOG_INFO(Engines, "DEBUG compute {:#x} images:{}", program_addr,
                     images);
        }
        static const std::string skip = [] {
            const char* env = std::getenv("HYDRA_SKIP_COMPUTE");
            return std::string(env != nullptr ? env : "");
        }();
        if (!skip.empty() &&
            skip.find(fmt::format("{:x}", program_addr)) != std::string::npos)
            return;
    }

    LOG_DEBUG(Engines,
              "Compute launch (program: {:#x}, grid: {}x{}x{}, block: "
              "{}x{}x{}, shared: {:#x}, textures: {}, images: {})",
              program_addr, qmd.gridDimX(), qmd.gridDimY(), qmd.gridDimZ(),
              qmd.blockDimX(), qmd.blockDimY(), qmd.blockDimZ(),
              qmd.sharedAlloc(), dispatch.textures.size(),
              dispatch.images.size());

    // DEBUG: record the launch in the frame trace
    if (FrameTracer::instance().isActive()) {
        std::string str = fmt::format(
            "#C compute program={:#x} grid={}x{}x{} block={}x{}x{} shared={:#x}\n",
            program_addr, qmd.gridDimX(), qmd.gridDimY(), qmd.gridDimZ(),
            qmd.blockDimX(), qmd.blockDimY(), qmd.blockDimZ(),
            qmd.sharedAlloc());
        for (const auto& [binding_index, renderer_index] :
             resource_mapping.images) {
            const auto tic = get_tic(binding_index);
            if (!tic)
                continue;
            if (tic->hdr_version != TicHdrVersion::_1DBuffer &&
                !tic->normalized_coords)
                descriptor.state.unnormalized_textures.push_back(binding_index);
            str += fmt::format(
                "    image {:#x}: addr={:#x} fmt={} {}x{}x{} hdr={}\n",
                binding_index, makeAddr(tic->addr_lo, tic->addr_hi),
                renderer::toTextureFormat(tic->format_word, tic->is_srgb),
                tic->width_minus_one + 1, tic->height_minus_one + 1,
                tic->depth_minus_one + 1, static_cast<u32>(tic->hdr_version));
        }
        auto& tracer = FrameTracer::instance();
        for (const auto& traced : traced_textures) {
            const auto format = renderer::toTextureFormat(
                traced.tic.format_word, traced.tic.is_srgb);
            str += fmt::format("    texture {:#x}: addr={:#x} fmt={} {}x{}x{}\n",
                               traced.binding_index,
                               makeAddr(traced.tic.addr_lo, traced.tic.addr_hi),
                               format, traced.tic.width_minus_one + 1,
                               traced.tic.height_minus_one + 1,
                               traced.tic.depth_minus_one + 1);
            if (!tracer.isDumpEnabled() ||
                traced.tic.hdr_version == TicHdrVersion::_1DBuffer ||
                !tracer.markDumped(traced.view))
                continue;
            const auto path =
                tracer.getDir() /
                fmt::format("f{}_c{:x}_tex{:x}_{}_{}x{}.png",
                            tracer.getFrameIndex(), program_addr,
                            traced.binding_index, format,
                            traced.tic.width_minus_one + 1,
                            traced.tic.height_minus_one + 1);
            if (tracer.allowImageDump())
                renderer.debugDumpTexture(tls_crnt_command_buffer, traced.view,
                                          path.string());
        }
        str += fmt::format("    textures={} images={}\n",
                           dispatch.textures.size(), dispatch.images.size());
        FrameTracer::instance().write(str);
    }

    renderer.dispatchCompute(tls_crnt_command_buffer, dispatch);
}

} // namespace hydra::hw::tegra_x1::gpu::engines

#include "core/hw/tegra_x1/gpu/engines/3d.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/frame_tracer.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

#include <fmt/ranges.h>
#include <fstream>
#include <set>
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/macro/interpreter/driver.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

namespace {

macro::DriverBase* createMacroDriver(ThreeD& three_d) {
    // TODO: choose based on Macro backend
    return new macro::interpreter::Driver(three_d);
}

u32 getImageHandle(u32 handle) { return extractBits(handle, 0, 20); }
u32 getSamplerHandle(u32 handle) { return extractBits(handle, 20, 12); }

renderer::TextureType toTextureType(TextureType type) {
    switch (type) {
    case TextureType::_1D:
        return renderer::TextureType::_1D;
    case TextureType::_1DArray:
        return renderer::TextureType::_1DArray;
    case TextureType::_1DBuffer:
        return renderer::TextureType::_1DBuffer;
    case TextureType::_2D:
    case TextureType::_2DNoMipmap:
        return renderer::TextureType::_2D;
    case TextureType::_2DArray:
        return renderer::TextureType::_2DArray;
    case TextureType::_3D:
        return renderer::TextureType::_3D;
    case TextureType::Cubemap:
        return renderer::TextureType::Cube;
    case TextureType::CubeArray:
        return renderer::TextureType::CubeArray;
    }
}

constexpr u32 GL_MIN = 0x8007;
constexpr u32 GL_MAX = 0x8008;
constexpr u32 GL_FUNC_ADD = 0x8006;
constexpr u32 GL_FUNC_SUBTRACT = 0x800A;
constexpr u32 GL_FUNC_REVERSE_SUBTRACT = 0x800B;

constexpr u32 D3D11_BLEND_OP_ADD = 1;
constexpr u32 D3D11_BLEND_OP_SUB = 2;
constexpr u32 D3D11_BLEND_OP_REV_SUB = 3;
constexpr u32 D3D11_BLEND_OP_MIN = 4;
constexpr u32 D3D11_BLEND_OP_MAX = 5;

renderer::BlendOperation getBlendOperation(u32 blend_op) {
    switch (blend_op) {
    // GL
    case GL_MIN:
        return renderer::BlendOperation::Min;
    case GL_MAX:
        return renderer::BlendOperation::Max;
    case GL_FUNC_ADD:
        return renderer::BlendOperation::Add;
    case GL_FUNC_SUBTRACT:
        return renderer::BlendOperation::Sub;
    case GL_FUNC_REVERSE_SUBTRACT:
        return renderer::BlendOperation::RevSub;

    // DX11
    case D3D11_BLEND_OP_ADD:
        return renderer::BlendOperation::Add;
    case D3D11_BLEND_OP_SUB:
        return renderer::BlendOperation::Sub;
    case D3D11_BLEND_OP_REV_SUB:
        return renderer::BlendOperation::RevSub;
    case D3D11_BLEND_OP_MIN:
        return renderer::BlendOperation::Min;
    case D3D11_BLEND_OP_MAX:
        return renderer::BlendOperation::Max;
    default:
        ONCE(LOG_ERROR(Engines, "Unknown blend operation 0x{:04x}", blend_op));
        return renderer::BlendOperation::Add;
    }
}

constexpr u32 GL_ZERO = 0;
constexpr u32 GL_ONE = 1;
constexpr u32 GL_SRC_COLOR = 0x0300;
constexpr u32 GL_ONE_MINUS_SRC_COLOR = 0x0301;
constexpr u32 GL_SRC_ALPHA = 0x0302;
constexpr u32 GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr u32 GL_DST_ALPHA = 0x0304;
constexpr u32 GL_ONE_MINUS_DST_ALPHA = 0x0305;
constexpr u32 GL_DST_COLOR = 0x0306;
constexpr u32 GL_ONE_MINUS_DST_COLOR = 0x0307;
constexpr u32 GL_SRC_ALPHA_SATURATE = 0x0308;
// TODO: more

constexpr u32 D3D11_BLEND_FACTOR_ZERO = 1;
constexpr u32 D3D11_BLEND_FACTOR_ONE = 2;
constexpr u32 D3D11_BLEND_FACTOR_SRC_COLOR = 3;
constexpr u32 D3D11_BLEND_FACTOR_INV_SRC_COLOR = 4;
constexpr u32 D3D11_BLEND_FACTOR_SRC_ALPHA = 5;
constexpr u32 D3D11_BLEND_FACTOR_INV_SRC_ALPHA = 6;
constexpr u32 D3D11_BLEND_FACTOR_DST_ALPHA = 7;
constexpr u32 D3D11_BLEND_FACTOR_INV_DST_ALPHA = 8;
constexpr u32 D3D11_BLEND_FACTOR_DST_COLOR = 9;
constexpr u32 D3D11_BLEND_FACTOR_INV_DST_COLOR = 10;
constexpr u32 D3D11_BLEND_FACTOR_SRC_ALPHA_SATURATE = 11;
constexpr u32 D3D11_BLEND_FACTOR_SRC1_COLOR = 16;
constexpr u32 D3D11_BLEND_FACTOR_INV_SRC1_COLOR = 17;
constexpr u32 D3D11_BLEND_FACTOR_SRC1_ALPHA = 18;
constexpr u32 D3D11_BLEND_FACTOR_INV_SRC1_ALPHA = 19;
// TODO: const color and alpha?

constexpr u32 GL_BLEND_FACTOR_BIT = 0x4000;

renderer::BlendFactor getBlendFactor(u32 blend_factor) {
    if ((blend_factor & GL_BLEND_FACTOR_BIT) != 0u) { // GL
        u32 gl_blend_factor = blend_factor & ~GL_BLEND_FACTOR_BIT;
        switch (gl_blend_factor) {
        case GL_ZERO:
            return renderer::BlendFactor::Zero;
        case GL_ONE:
            return renderer::BlendFactor::One;
        case GL_SRC_COLOR:
            return renderer::BlendFactor::SrcColor;
        case GL_ONE_MINUS_SRC_COLOR:
            return renderer::BlendFactor::InvSrcColor;
        case GL_SRC_ALPHA:
            return renderer::BlendFactor::SrcAlpha;
        case GL_ONE_MINUS_SRC_ALPHA:
            return renderer::BlendFactor::InvSrcAlpha;
        case GL_DST_ALPHA:
            return renderer::BlendFactor::DstAlpha;
        case GL_ONE_MINUS_DST_ALPHA:
            return renderer::BlendFactor::InvDstAlpha;
        case GL_DST_COLOR:
            return renderer::BlendFactor::DstColor;
        case GL_ONE_MINUS_DST_COLOR:
            return renderer::BlendFactor::InvDstColor;
        case GL_SRC_ALPHA_SATURATE:
            return renderer::BlendFactor::SrcAlphaSaturate;
        default:
            ONCE(LOG_ERROR(Engines, "Unknown GL blend factor 0x{:04x}",
                           gl_blend_factor));
            return renderer::BlendFactor::One;
        }
    } else { // D3D11
        switch (blend_factor) {
        case D3D11_BLEND_FACTOR_ZERO:
            return renderer::BlendFactor::Zero;
        case D3D11_BLEND_FACTOR_ONE:
            return renderer::BlendFactor::One;
        case D3D11_BLEND_FACTOR_SRC_COLOR:
            return renderer::BlendFactor::SrcColor;
        case D3D11_BLEND_FACTOR_INV_SRC_COLOR:
            return renderer::BlendFactor::InvSrcColor;
        case D3D11_BLEND_FACTOR_SRC_ALPHA:
            return renderer::BlendFactor::SrcAlpha;
        case D3D11_BLEND_FACTOR_INV_SRC_ALPHA:
            return renderer::BlendFactor::InvSrcAlpha;
        case D3D11_BLEND_FACTOR_DST_ALPHA:
            return renderer::BlendFactor::DstAlpha;
        case D3D11_BLEND_FACTOR_INV_DST_ALPHA:
            return renderer::BlendFactor::InvDstAlpha;
        case D3D11_BLEND_FACTOR_DST_COLOR:
            return renderer::BlendFactor::DstColor;
        case D3D11_BLEND_FACTOR_INV_DST_COLOR:
            return renderer::BlendFactor::InvDstColor;
        case D3D11_BLEND_FACTOR_SRC_ALPHA_SATURATE:
            return renderer::BlendFactor::SrcAlphaSaturate;
        case D3D11_BLEND_FACTOR_SRC1_COLOR:
            return renderer::BlendFactor::Src1Color;
        case D3D11_BLEND_FACTOR_INV_SRC1_COLOR:
            return renderer::BlendFactor::InvSrc1Color;
        case D3D11_BLEND_FACTOR_SRC1_ALPHA:
            return renderer::BlendFactor::Src1Alpha;
        case D3D11_BLEND_FACTOR_INV_SRC1_ALPHA:
            return renderer::BlendFactor::InvSrc1Alpha;
        default:
            ONCE(LOG_ERROR(Engines, "Unknown D3D11 blend factor 0x{:04x}",
                           blend_factor));
            return renderer::BlendFactor::One;
        }
    }
}

// Render target width is aligned to the stride, lets try to figure out the real
// one
u32 getMinimumWidth(u32 width, renderer::TextureFormat format, u32 width_hint) {
    if (width <= width_hint)
        return width;

    // Get the smallest width that would still align up to the same GOB
    // count
    const auto bpp = renderer::getTextureFormatBpp(format);
    const auto alignment = 64 / bpp;
    const auto width_aligned = align(width, alignment);
    // HACK
    // return std::clamp(width_aligned - alignment + 1, width_hint,
    // width_aligned);
    if (width_aligned - alignment + 1 <= width_hint)
        return width_hint;
    return width;
}

} // namespace

DEFINE_METHOD_TABLE(ThreeD, INLINE_ENGINE_TABLE, 0x45, 1,
                    loadMmeInstructionRamPointer, u32, 0x46, 1,
                    loadMmeInstructionRam, u32, 0x47, 1,
                    loadMmeStartAddressRamPointer, u32, 0x48, 1,
                    loadMmeStartAddressRam, u32, 0x35e, 1, drawVertexArray, u32,
                    0x586, 1, beginDraw, u32,
                    0x5f8, 1, drawVertexElements, u32, 0x674, 1, clearBuffer,
                    ClearBufferData, 0x6c3, 1, setReportSemaphore, u32, 0x8c4,
                    1, firmwareCall4, u32, 0x8e4, 16, loadConstBuffer, u32,
                    0x900, 5 * 8, bindGroup, u32)

ThreeD::ThreeD(Gpu& gpu_) : gpu{gpu_}, macro_driver{createMacroDriver(*this)} {
    // Initialize default state

    // Viewports
    // TODO: correct?
    for (auto& viewport_transform : regs.viewport_transforms) {
        viewport_transform.swizzle = {
            .x = ViewportSwizzle::PositiveX,
            .y = ViewportSwizzle::PositiveY,
            .z = ViewportSwizzle::PositiveZ,
            .w = ViewportSwizzle::PositiveW,
        };
    }

    // Color write masks
    // TODO: correct?
    for (auto& color_write_mask : regs.color_write_masks)
        color_write_mask = ColorWriteMask::All;

    // HACK
    regs.shader_programs[static_cast<u32>(ShaderStage::VertexB)].config.enable =
        true;
}

void ThreeD::flushMacro() { macro_driver->execute(); }

void ThreeD::macro(u32 method, u32 arg) {
    u32 index = (method - MACRO_METHODS_REGION) >> 1;
    LOG_DEBUG(Engines, "Macro (index: 0x{:08x})", index);
    if ((method & 0x1) == 0x0) {
        LOG_DEBUG(Engines, "Parameter1: 0x{:08x}", arg);
        macro_driver->setIndex(index);
        macro_driver->loadParam1(arg);
    } else {
        LOG_DEBUG(Engines, "ParameterN: 0x{:08x}", arg);
        macro_driver->loadParam(arg);
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void ThreeD::loadMmeInstructionRamPointer(const u32 index, const u32 ptr) {
    macro_driver->loadInstructionRamPointer(ptr);
}

void ThreeD::loadMmeInstructionRam(const u32 index, const u32 data) {
    macro_driver->loadInstructionRam(data);
}

void ThreeD::loadMmeStartAddressRamPointer(const u32 index, const u32 ptr) {
    macro_driver->loadStartAddressRamPointer(ptr);
}

void ThreeD::loadMmeStartAddressRam(const u32 index, const u32 data) {
    macro_driver->loadStartAddressRam(data);
}

void ThreeD::beginDraw(const u32 index, const u32 arg) {
    writeReg(0x586, arg);

    // DEBUG: log begin values while rendering to a 3D target
    if (regs.color_targets[0].tile_mode.is_3d) {
        static u32 logs = 0;
        if (logs++ < 100)
            LOG_INFO(Engines, "DEBUG begin {:#010x} base_instance {}", arg,
                     regs.base_instance);
    }

    // Instanced draws are submitted as one draw per instance
    switch (extractBits(arg, 26, 2)) {
    case 0: // First
        instance_index = 0;
        break;
    case 1: // Subsequent
        instance_index++;
        break;
    default: // Unchanged
        break;
    }
}

void ThreeD::drawVertexArray(const u32 index, u32 count) {
    auto index_type = IndexType::None;
    auto primitive_type = regs.begin.primitive_type;
    renderer::BufferView index_buffer;
    {
        std::scoped_lock buffer_cache_lock(
            gpu.getRenderer().getBufferCache().getMutex());
        if (!drawInternal())
            return;
        if (FrameTracer::instance().isActive())
            traceDraw("draw", count, regs.vertex_array_start, 0);

        index_buffer = gpu.getRenderer().getIndexCache().decode(
            tls_crnt_command_buffer,
            {.type = index_type,
             .primitive_type = primitive_type,
             .count = count},
            index_type, primitive_type, count);
    }

    if (index_buffer.getBase() != nullptr) {
        // Bind index buffer
        gpu.getRenderer().bindIndexBuffer(index_buffer, index_type);

        // Draw

        // Vertex start is set as vertex base instead, as start is now index
        // start
        // TODO: instance count
        gpu.getRenderer().drawIndexed(tls_crnt_command_buffer, primitive_type,
                                      0, count, regs.vertex_array_start,
                                      regs.base_instance + instance_index, 1);
        profileDraw(count);
    } else {
        // Draw
        // TODO: instance count
        gpu.getRenderer().draw(tls_crnt_command_buffer, primitive_type,
                               regs.vertex_array_start, count,
                               regs.base_instance + instance_index, 1);
        profileDraw(count);
    }
}

// DEBUG: GPU time per shader pair, enabled with HYDRA_PROFILE_DRAWS
void ThreeD::profileDraw(u32 count) {
    static const bool enabled = std::getenv("HYDRA_PROFILE_DRAWS") != nullptr;
    if (!enabled)
        return;
    const auto ms =
        gpu.getRenderer().debugSyncCommandBuffer(tls_crnt_command_buffer);
    const auto program_addr = [&](ShaderStage stage) {
        return static_cast<u64>(regs.shader_program_region) +
               regs.shader_programs[static_cast<usize>(stage)].offset;
    };
    struct Entry {
        f64 total_ms{0.0};
        f64 max_ms{0.0};
        u32 draws{0};
        u32 max_count{0};
    };
    static std::map<std::pair<u64, u64>, Entry> entries;
    static auto last = std::chrono::steady_clock::now();
    auto& entry = entries[{program_addr(ShaderStage::VertexB),
                           program_addr(ShaderStage::Fragment)}];
    entry.total_ms += ms;
    entry.max_ms = std::max(entry.max_ms, ms);
    entry.draws++;
    entry.max_count = std::max(entry.max_count, count);

    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5))
        return;
    last = now;
    std::vector<std::pair<std::pair<u64, u64>, Entry>> sorted(entries.begin(),
                                                              entries.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second.total_ms > b.second.total_ms;
    });
    for (usize i = 0; i < std::min<usize>(sorted.size(), 12); i++) {
        const auto& [key, e] = sorted[i];
        LOG_INFO(Engines,
                 "DEBUG draw profile vs {:#x} fs {:#x}: {:.1f} ms total, {:.2f} "
                 "ms max, {} draws, max count {}",
                 key.first, key.second, e.total_ms, e.max_ms, e.draws,
                 e.max_count);
    }
    entries.clear();
}

void ThreeD::drawVertexElements(const u32 index, u32 count) {
    auto index_type = regs.index_type;
    auto primitive_type = regs.begin.primitive_type;
    renderer::BufferView index_buffer;
    {
        std::scoped_lock buffer_cache_lock(
            gpu.getRenderer().getBufferCache().getMutex());
        if (!drawInternal())
            return;
        if (FrameTracer::instance().isActive())
            traceDraw("drawIndexed", count, regs.vertex_elements_start,
                      regs.base_vertex);

        // Index buffer
        gpu_vaddr_t index_buffer_ptr =
            tls_crnt_gmmu->unmapAddr(regs.index_buffer_addr);
        // TODO: uncomment?
        u32 index_buffer_size =
            count *
            getIndexTypeSize(
                regs.index_type); // u64(regs.index_buffer_limit_addr) + 1
                                  // - u64(regs.index_buffer_addr);
        const auto range =
            ztd::Range<uptr>::fromSize(index_buffer_ptr, index_buffer_size);

        index_buffer = gpu.getRenderer().getIndexCache().decode(
            tls_crnt_command_buffer,
            {.type = index_type,
             .primitive_type = primitive_type,
             .count = count,
             .mem_range = range},
            index_type, primitive_type, count);
    }

    // Bind index buffer
    ASSERT_DEBUG(index_buffer.getBase(), Gpu, "Index buffer not found");
    gpu.getRenderer().bindIndexBuffer(index_buffer, index_type);

    // Draw
    // TODO: instance count
    gpu.getRenderer().drawIndexed(tls_crnt_command_buffer, primitive_type,
                                  regs.vertex_elements_start, count,
                                  regs.base_vertex,
                                  regs.base_instance + instance_index, 1);
    profileDraw(count);
}

void ThreeD::clearBuffer(const u32 index, const ClearBufferData data) {
    LOG_DEBUG(Gpu,
              "Depth: {}, stencil: {}, color mask: 0x{:x}, target id: {}, "
              "layer id: {}",
              data.depth, data.stencil, data.color_mask, data.target_id,
              data.layer_id);

    // DEBUG: trace clears
    if (FrameTracer::instance().isActive())
        FrameTracer::instance().write(fmt::format(
            "#clear color_mask={:#x} target={} depth={} stencil={} "
            "clear_depth={} clear_color=({}, {}, {}, {})\n",
            static_cast<u32>(data.color_mask), static_cast<u32>(data.target_id),
            static_cast<bool>(data.depth), static_cast<bool>(data.stencil),
            regs.clear_depth, regs.clear_color.x(), regs.clear_color.y(),
            regs.clear_color.z(), regs.clear_color.w()));

    // Deferred clear
    // TODO: implement

    // Regular clear
    {
        std::scoped_lock texture_cache_lock(
            gpu.getRenderer().getTextureCache().getMutex());
        gpu.getRenderer().bindRenderPass(getRenderPass());
    }

    if (data.color_mask != 0x0)
        gpu.getRenderer().clearColor(tls_crnt_command_buffer, data.target_id,
                                     data.layer_id, data.color_mask,
                                     regs.clear_color);

    if (data.depth)
        gpu.getRenderer().clearDepth(tls_crnt_command_buffer, data.layer_id,
                                     regs.clear_depth);

    if (data.stencil)
        gpu.getRenderer().clearStencil(tls_crnt_command_buffer, data.layer_id,
                                       regs.clear_stencil);
}

void ThreeD::setReportSemaphore(const u32 index, const u32 data) {
    // Query control word
    const u32 operation = extractBits(data, 0, 2);
    const u32 counter_type = extractBits(data, 23, 5);
    const bool short_query = extractBits(data, 28, 1) != 0;

    // DEBUG: log the kinds of queries used
    {
        static std::mutex debug_mutex;
        static std::set<u32> seen;
        std::scoped_lock lock(debug_mutex);
        if (seen.size() < 32 && seen.insert(data & 0xfffffff3u).second)
            LOG_INFO(Engines,
                     "DEBUG semaphore: data={:#010x} op={} counter={:#x} "
                     "short={} payload={:#x}",
                     data, operation, counter_type, short_query,
                     regs.report_semaphore_payload);
    }

    u64 value;
    switch (operation) {
    case 0: // Release
        value = regs.report_semaphore_payload;
        break;
    case 1: // Acquire
        // The GPU executes in order, nothing to wait for
        return;
    case 2: // Counter
        switch (counter_type) {
        case 0x0: // Payload
            value = regs.report_semaphore_payload;
            break;
        case 0x2:  // ZPassPixelCount
        case 0xe:  // SamplesPassed (approximately)
        case 0xf:
            // HACK: occlusion queries are not implemented, report as visible
            value = 1;
            break;
        default:
            value = 0;
            break;
        }
        break;
    default:
        ONCE(LOG_NOT_IMPLEMENTED(Engines, "Semaphore operation {}", operation));
        return;
    }

    const uptr ptr = tls_crnt_gmmu->unmapAddr(regs.report_semaphore_addr);
    if (short_query) {
        *reinterpret_cast<u32*>(ptr) = static_cast<u32>(value);
    } else {
        // GPU timestamp (ticks at 614.4 MHz derived from nanoseconds)
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const u64 timestamp = ns / 625 * 384;
        reinterpret_cast<u64*>(ptr)[0] = value;
        reinterpret_cast<u64*>(ptr)[1] = timestamp;
    }
}

void ThreeD::firmwareCall4(const u32 index, const u32 data) {
    ONCE(LOG_FUNC_STUBBED(Engines));

    // TODO: find out what this does
    regs.mme_scratch[0] = 0x1;
}

void ThreeD::loadConstBuffer(const u32 index, const u32 data) {
    const uptr const_buffer_gpu_addr =
        static_cast<u64>(regs.const_buffer_selector);
    const uptr gpu_addr = const_buffer_gpu_addr + regs.load_const_buffer_offset;
    const auto ptr = tls_crnt_gmmu->unmapAddr(gpu_addr);

    *reinterpret_cast<u32*>(ptr) = data;
    regs.load_const_buffer_offset += sizeof(u32);

    // Invalidate
    // TODO: invalidate as a whole
    gpu.getRenderer().invalidateMemory(
        ztd::Range<uptr>::fromSize(ptr, sizeof(u32)),
        renderer::MemoryInvalidationScope::BufferCache);
}

void ThreeD::bindGroup(const u32 index, const u32 data) {
    const auto shader_stage_index = index / 0x8;
    const auto group = index % 0x8;

    switch (group) {
    case 0x0 ... 0x3:
        LOG_WARN(Engines, "Reserved");
        break;
    case 0x4: {
        const auto buffer_index = extractBits(data, 4, 5);
        bool valid = (data & 0x1) != 0u;
        if (valid) {
            const uptr const_buffer_gpu_ptr =
                tls_crnt_gmmu->unmapAddr(regs.const_buffer_selector);

            const auto range = ztd::Range<uptr>::fromSize(
                const_buffer_gpu_ptr, regs.const_buffer_selector_size);
            bound_const_buffers[shader_stage_index][buffer_index] = range;
        } else {
            bound_const_buffers[shader_stage_index][buffer_index] =
                ztd::Range<uptr>();
        }
        break;
    }
    default:
        LOG_WARN(Engines, "Unknown group {}", group);
        break;
    }
}

#pragma GCC diagnostic pop

renderer::ITextureView*
ThreeD::getColorTargetTexture(u32 render_target_index) const {
    const auto& render_target = regs.color_targets[render_target_index];

    const auto gpu_addr = static_cast<u64>(render_target.addr);
    if (gpu_addr == 0x0) {
        // TODO: is this really an error?
        LOG_ERROR(Engines, "Invalid color render target at index {}",
                  render_target_index);
        return nullptr;
    }

    const auto format = renderer::toTextureFormat(render_target.format);

    // Depth and layer count
    auto type = renderer::TextureType::_2D;
    u32 depth = 1;
    u32 layer_count = 1;
    if (render_target.tile_mode.is_3d) {
        type = renderer::TextureType::_3D;
        depth = render_target.array_mode.layers;
    } else {
        layer_count = render_target.array_mode.layers;
        if (layer_count > 1)
            type = renderer::TextureType::_2DArray;
    }

    // Width and stride
    const bool is_linear = render_target.tile_mode.is_linear;
    u32 width;
    u32 stride;
    if (is_linear) {
        width = render_target.width_or_stride /
                renderer::getTextureFormatBpp(format);
        stride = render_target.width_or_stride;
    } else {
        const u32 width_hint = regs.screen_scissor.horizontal.x +
                               regs.screen_scissor.horizontal.width;
        width =
            getMinimumWidth(render_target.width_or_stride, format, width_hint);
        stride = 0;
    }

    const renderer::TextureDescriptor descriptor(
        tls_crnt_gmmu->unmapAddr(gpu_addr), type, format, is_linear, stride,
        width, render_target.height, depth, 1, layer_count,
        render_target.tile_mode.width_gobs_log2,
        render_target.tile_mode.height_gobs_log2,
        render_target.tile_mode.depth_gobs_log2,
        render_target.layer_stride * 4);

    return gpu.getRenderer().getTextureCache().find(
        tls_crnt_command_buffer, descriptor, renderer::TextureUsage::Write);
}

renderer::ITextureView* ThreeD::getDepthStencilTargetTexture() const {
    const auto gpu_addr = static_cast<u64>(regs.depth_target_addr);
    if (gpu_addr == 0x0) {
        // TODO: is this really an error?
        LOG_ERROR(Engines, "Invalid depth render target");
        return nullptr;
    }

    const auto type = regs.depth_target_array_mode.layers > 1
                          ? renderer::TextureType::_2DArray
                          : renderer::TextureType::_2D;

    const renderer::TextureDescriptor descriptor(
        tls_crnt_gmmu->unmapAddr(gpu_addr), type,
        renderer::toTextureFormat(regs.depth_target_format), false, 0,
        regs.depth_target_width, regs.depth_target_height, 1, 1,
        regs.depth_target_array_mode.layers,
        regs.depth_target_tile_mode.width_gobs_log2,
        regs.depth_target_tile_mode.height_gobs_log2,
        regs.depth_target_tile_mode.depth_gobs_log2,
        regs.depth_target_layer_stride * 4);

    return gpu.getRenderer().getTextureCache().find(
        tls_crnt_command_buffer, descriptor, renderer::TextureUsage::Write);
}

renderer::RenderPassBase* ThreeD::getRenderPass() const {
    renderer::RenderPassDescriptor descriptor{};

    // Color targets
    for (u32 i = 0; i < regs.color_target_control.count; i++) {
        descriptor.color_targets[i] = {
            .texture =
                getColorTargetTexture(regs.color_target_control.getMap(i)),
        };
    }

    // Depth stencil target
    descriptor.depth_stencil_target = {
        .texture =
            ((regs.depth_target_enabled != 0u) ? getDepthStencilTargetTexture()
                                               : nullptr),
    };

    return gpu.getRenderer().getRenderPassCache().find(descriptor);
}

renderer::Viewport ThreeD::getViewport(u32 index) {
    renderer::Viewport res;

    const auto& extent = regs.viewports[index];
    const auto& transform = regs.viewport_transforms[index];
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (/*regs.viewport_transform_enabled*/ true) { // HACK
        auto scale_x = transform.scale_x;
        auto scale_y = transform.scale_y;
        if (any(regs.window_origin_flags &
                engines::WindowOriginFlags::LowerLeft))
            scale_y = -scale_y;

        // Swizzle
        // TODO: check for viewport swizzle support
        if (transform.swizzle.x == engines::ViewportSwizzle::NegativeX) {
            scale_x = -scale_x;
        } else {
            ASSERT_DEBUG(transform.swizzle.x ==
                             engines::ViewportSwizzle::PositiveX,
                         Engines, "Unsupported X viewport swizzle {}",
                         transform.swizzle.x);
        }
        if (transform.swizzle.y == engines::ViewportSwizzle::NegativeY) {
            scale_y = -scale_y;
        } else {
            ASSERT_DEBUG(transform.swizzle.y ==
                             engines::ViewportSwizzle::PositiveY,
                         Engines, "Unsupported Y viewport swizzle {}",
                         transform.swizzle.y);
        }
        ASSERT_DEBUG(transform.swizzle.z == engines::ViewportSwizzle::PositiveZ,
                     Engines, "Unsupported Z viewport swizzle {}",
                     transform.swizzle.z);
        ASSERT_DEBUG(transform.swizzle.w == engines::ViewportSwizzle::PositiveW,
                     Engines, "Unsupported W viewport swizzle {}",
                     transform.swizzle.w);

        res.rect.origin.x() = transform.offset_x - scale_x;
        res.rect.origin.y() = transform.offset_y - scale_y;
        res.rect.size.x() = scale_x * 2.0f;
        res.rect.size.y() = scale_y * 2.0f;
        // Depth range from the Z scale and offset
        const bool zero_to_one =
            regs.viewport_z_clip == ViewportZClip::ZeroToOne;
        ONCE(LOG_INFO(Engines, "Depth mode: {}",
                      zero_to_one ? "zero to one" : "minus one to one"));
        if (transform.scale_z != 0.0f || transform.offset_z != 0.0f) {
            res.depth_near =
                transform.offset_z - (zero_to_one ? 0.0f : transform.scale_z);
            res.depth_far = transform.offset_z + transform.scale_z;
        } else {
            res.depth_near = extent.near;
            res.depth_far = extent.far;
        }
    } else {
        const auto& screen_scissor = regs.screen_scissor;
        res.rect.origin.x() = screen_scissor.horizontal.x;
        res.rect.origin.y() = screen_scissor.vertical.y;
        res.rect.size.x() = screen_scissor.horizontal.width;
        res.rect.size.y() = screen_scissor.vertical.height;
        res.depth_near = extent.near;
        res.depth_far = extent.far;
    }

    // Flip Y
    res.rect.origin.y() += res.rect.size.y();
    res.rect.size.y() = -res.rect.size.y();

    // HACK: if depth range is [0, 0], force it to [0, 1] (many games have
    // it like this, though not on Ryujinx)
    if (res.depth_near == 0.0f && res.depth_far == 0.0f) {
        ONCE(LOG_WARN(Engines, "Depth range is [0, 0], forcing to [0, 1]"));
        res.depth_near = 0.0f;
        res.depth_far = 1.0f;
    }

    return res;
}

renderer::Scissor ThreeD::getScissor(u32 index) {
    const auto& scissor = regs.scissors[index];
    if (scissor.enabled != 0u) {
        return renderer::Scissor(
            uint2({scissor.horizontal.min, scissor.vertical.min}),
            uint2({static_cast<u32>(scissor.horizontal.max -
                                    scissor.horizontal.min),
                   static_cast<u32>(scissor.vertical.max -
                                    scissor.vertical.min)}));
    } else {
        return renderer::Scissor(uint2({0, 0}), uint2({0xffff, 0xffff}));
    }
}

renderer::ShaderBase* ThreeD::getShaderUnchecked(ShaderStage stage) const {
    return active_shaders[static_cast<u32>(toRendererShaderType(stage))];
}

namespace {

// DEBUG: highlight draws that use this vertex shader code address
u64 getHighlightVsAddr() {
    static const u64 addr = [] {
        const char* env = std::getenv("HYDRA_HIGHLIGHT_VS");
        return env != nullptr ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    return addr;
}

} // namespace

bool ThreeD::isHighlightedDraw() const {
    // DEBUG: also match a fragment shader code address
    static const u64 fs_addr = [] {
        const char* env = std::getenv("HYDRA_HIGHLIGHT_FS");
        return env != nullptr ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    const auto program_addr = [&](ShaderStage stage) {
        return static_cast<u64>(regs.shader_program_region) +
               regs.shader_programs[static_cast<usize>(stage)].offset;
    };

    const auto addr = getHighlightVsAddr();
    if (addr != 0 && program_addr(ShaderStage::VertexB) == addr)
        return true;
    return fs_addr != 0 && program_addr(ShaderStage::Fragment) == fs_addr;
}

renderer::ShaderBase* ThreeD::getShader(ShaderStage stage,
                                        u32 fragment_input_mask) {
    const auto& program = regs.shader_programs[static_cast<usize>(stage)];
    if (!program.config.enable) {
        active_shaders[static_cast<u32>(toRendererShaderType(stage))] = nullptr;
        return nullptr;
    }

    uptr gpu_addr =
        static_cast<u64>(regs.shader_program_region) + program.offset;
    uptr ptr = tls_crnt_gmmu->unmapAddr(gpu_addr);

    renderer::GuestShaderDescriptor descriptor{
        .stage = stage,
        .code_ptr = ptr,
        .gpu_addr = gpu_addr,
    };

    // DEBUG: dump the guest shader binary (header + code)
    static const char* dump_dir = std::getenv("HYDRA_DUMP_SHADERS");
    if (dump_dir != nullptr) {
        static std::mutex dump_mutex;
        static std::set<uptr> dumped;
        std::scoped_lock lock(dump_mutex);
        if (dumped.insert(gpu_addr).second) {
            std::ofstream out(fmt::format("{}/guest_{:#x}.bin", dump_dir,
                                          static_cast<u64>(gpu_addr)),
                              std::ios::binary);
            const auto bytes = tls_crnt_gmmu->readMappedBytes(gpu_addr, 0x10000);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
    }
    if (stage == ShaderStage::VertexB) {
        descriptor.state.fragment_input_mask = fragment_input_mask;
        descriptor.state.depth_zero_to_one =
            regs.viewport_z_clip == ViewportZClip::ZeroToOne;
    }
    if (stage == ShaderStage::Fragment)
        descriptor.state.debug_highlight = isHighlightedDraw();

    // Vertex attribute states
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        descriptor.state.vertex_attrib_states[i] = regs.vertex_attrib_states[i];
    }

    // DEBUG: log the attributes of a vertex shader (HYDRA_LOG_ATTRIBS_VS)
    static const u64 log_attribs_vs = [] {
        const char* env = std::getenv("HYDRA_LOG_ATTRIBS_VS");
        return env != nullptr ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    if (stage == ShaderStage::VertexB && log_attribs_vs != 0 &&
        (log_attribs_vs == 1 || gpu_addr == log_attribs_vs)) {
        static std::set<std::string> logged;
        std::string str;
        for (u32 i = 0; i < 6; i++) {
            const auto& a = regs.vertex_attrib_states[i];
            str += fmt::format(" [{}: buf {} off {} size {:#x} type {} fixed "
                               "{} bgra {} stride {}]",
                               i, a.buffer_id, a.offset,
                               static_cast<u32>(a.size),
                               static_cast<u32>(a.type), a.is_fixed, a.bgra,
                               static_cast<u32>(
                                   regs.vertex_arrays[a.buffer_id].config.stride));
        }
        if (logged.insert(str).second)
            LOG_INFO(Engines, "DEBUG attribs vs {:#x}:{}", gpu_addr, str);
        // Raw data of attribute 4 for the first draws
        static u32 data_logs = 0;
        const auto& a4 = regs.vertex_attrib_states[4];
        const auto& array = regs.vertex_arrays[a4.buffer_id];
        if (!a4.is_fixed && a4.buffer_id == 1 &&
            static_cast<u64>(array.addr) != 0 &&
            data_logs++ < 40) {
            const auto* data = reinterpret_cast<const u32*>(
                tls_crnt_gmmu->unmapAddr(array.addr));
            std::string words;
            for (u32 v = 0; v < 6; v++) {
                const auto* w =
                    data + (v * array.config.stride + a4.offset) / 4;
                words += fmt::format(" ({:08x} {:08x} {:08x} {:08x})", w[0],
                                     w[1], w[2], w[3]);
            }
            LOG_INFO(Engines,
                     "DEBUG attr4 vs {:#x} per_instance {} divisor {} start {} "
                     "base_instance {}:{}",
                     gpu_addr, regs.is_vertex_array_per_instance[a4.buffer_id],
                     array.divisor, regs.vertex_array_start, regs.base_instance,
                     words);
        }
    }

    // Color target formats
    for (u32 i = 0; i < regs.color_target_control.count; i++) {
        const auto& render_target =
            regs.color_targets[regs.color_target_control.getMap(i)];
        const auto addr = static_cast<u64>(render_target.addr);
        if (addr == 0x0)
            continue;

        descriptor.state.color_target_data_types[i] =
            renderer::toColorDataType(render_target.format);
    }

    auto& active_shader =
        active_shaders[static_cast<u32>(toRendererShaderType(stage))];
    active_shader = gpu.getRenderer().getShaderCache().find(descriptor);
    if (active_shader == nullptr)
        return nullptr;

    // Buffer textures require a texture_buffer<T> declaration in Metal.
    // Use the cached shader's actual resource list to specialize only bindings
    // that it accesses, rather than inspecting unrelated uniform data.
    const auto pool_addr = static_cast<u64>(regs.tex_header_pool);
    const auto stage_index = static_cast<u32>(stage) - 1;
    if (pool_addr != 0) {
        const auto* pool = reinterpret_cast<const TextureImageControl*>(
            tls_crnt_gmmu->unmapAddr(pool_addr));
        const auto& resource_mapping =
            active_shader->getDescriptor().resource_mapping;
        for (const auto& [index, renderer_index] : resource_mapping.textures) {
            const auto handle = readTextureHandle(stage_index, index);
            if (!handle)
                continue;
            // Only 1D accesses can be buffer textures. Games can leave stale
            // buffer descriptors in slots the shader samples as another type.
            const auto type_it = resource_mapping.texture_types.find(index);
            if (type_it == resource_mapping.texture_types.end() ||
                type_it->second != renderer::TextureType::_1D)
                continue;
            const auto& tic = pool[getImageHandle(*handle)];
            if (tic.hdr_version == TicHdrVersion::_1DBuffer)
                descriptor.state.buffer_texture_formats.emplace(
                    index,
                    renderer::toTextureFormat(tic.format_word, tic.is_srgb));
        }
        if (!descriptor.state.buffer_texture_formats.empty())
            active_shader = gpu.getRenderer().getShaderCache().find(descriptor);
    }

    return active_shader;
}

renderer::PipelineBase* ThreeD::getPipeline() {
    renderer::PipelineDescriptor descriptor{};

    // DEBUG: report enabled stages that are not supported
    for (const auto stage : {ShaderStage::VertexA, ShaderStage::TessCtrl,
                             ShaderStage::TessEval, ShaderStage::Geometry}) {
        const auto& program = regs.shader_programs[static_cast<usize>(stage)];
        if (!program.config.enable)
            continue;
        static std::set<std::pair<u32, u64>> reported;
        const auto addr = static_cast<u64>(regs.shader_program_region) +
                          program.offset;
        if (reported.insert({static_cast<u32>(stage), addr}).second)
            LOG_WARN(Engines,
                     "DEBUG unsupported stage {} enabled (code {:#x}, vs "
                     "{:#x}, rt0 layers {}, base layer {})",
                     stage, addr,
                     static_cast<u64>(regs.shader_program_region) +
                         regs.shader_programs[static_cast<usize>(
                                                  ShaderStage::VertexB)]
                             .offset,
                     static_cast<u32>(regs.color_targets[0].array_mode.layers),
                     regs.color_targets[0].base_layer);
    }

    // Shaders
    // TODO: add all shaders
    // The fragment shader comes first, since the vertex shader has to write
    // all of its inputs
    const auto fragment_shader = getShader(ShaderStage::Fragment);
    descriptor.shaders[static_cast<u32>(renderer::ShaderType::Fragment)] =
        fragment_shader;
    descriptor.shaders[static_cast<u32>(renderer::ShaderType::Vertex)] =
        getShader(ShaderStage::VertexB,
                  fragment_shader != nullptr
                      ? fragment_shader->getDescriptor()
                            .resource_mapping.stage_input_mask
                      : 0);

    // Vertex state

    // Vertex attribute states
    bool vertex_arrays_used[VERTEX_ARRAY_COUNT] = {false};
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        const auto& state = regs.vertex_attrib_states[i];
        descriptor.vertex_state.vertex_attrib_states[i] = state;

        // Set used vertex array
        if (state.type == engines::VertexAttribType::None)
            continue;

        // HACK: how are attributes disabled?
        if (state.is_fixed)
            continue;

        vertex_arrays_used[state.buffer_id] = true;
    }

    // Vertex arrays
    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++) {
        if (!vertex_arrays_used[i])
            continue;

        const auto& vertex_array = regs.vertex_arrays[i];
        descriptor.vertex_state.vertex_arrays[i] = {
            .enable = static_cast<bool>(vertex_array.config.enable),
            .stride = vertex_array.config.stride,
            .is_per_instance =
                static_cast<bool>(regs.is_vertex_array_per_instance[i]),
            .divisor = vertex_array.divisor,
        };
    }

    // Color targets
    ASSERT_DEBUG(!regs.advanced_blend_enabled, Engines,
                 "Advanced blending not implemented");

    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        auto& color_target = descriptor.color_target_states[i];
        // Must match the render pass attachments (see getRenderPass)
        color_target.format = renderer::TextureFormat::Invalid;
        if (i < regs.color_target_control.count) {
            const auto& render_target =
                regs.color_targets[regs.color_target_control.getMap(i)];
            if (static_cast<u64>(render_target.addr) != 0x0)
                color_target.format =
                    renderer::toTextureFormat(render_target.format);
        }
        color_target.write_mask = regs.color_write_masks[i];
        color_target.blend_enabled =
            static_cast<bool>(regs.color_blend_enabled[i]);
        if (color_target.blend_enabled) {
            if (regs.independent_blend_enabled != 0u) {
                const auto& blend_state = regs.independent_blend_state[i];
                color_target.rgb_op = getBlendOperation(blend_state.rgb_op);
                color_target.src_rgb_factor =
                    getBlendFactor(blend_state.src_rgb_factor);
                color_target.dst_rgb_factor =
                    getBlendFactor(blend_state.dst_rgb_factor);
                color_target.alpha_op = getBlendOperation(blend_state.alpha_op);
                color_target.src_alpha_factor =
                    getBlendFactor(blend_state.src_alpha_factor);
                color_target.dst_alpha_factor =
                    getBlendFactor(blend_state.dst_alpha_factor);
            } else {
                const auto& blend_state = regs.blend_state;
                color_target.rgb_op = getBlendOperation(blend_state.rgb_op);
                color_target.src_rgb_factor =
                    getBlendFactor(blend_state.src_rgb_factor);
                color_target.dst_rgb_factor =
                    getBlendFactor(blend_state.dst_rgb_factor);
                color_target.alpha_op = getBlendOperation(blend_state.alpha_op);
                color_target.src_alpha_factor =
                    getBlendFactor(blend_state.src_alpha_factor);
                color_target.dst_alpha_factor =
                    getBlendFactor(blend_state.dst_alpha_factor);
            }
        }
    }

    // Depth stencil target (must match the render pass)
    if (regs.depth_target_enabled != 0u &&
        static_cast<u64>(regs.depth_target_addr) != 0x0)
        descriptor.depth_stencil_format =
            renderer::toTextureFormat(regs.depth_target_format);

    return gpu.getRenderer().getPipelineCache().find(descriptor);
}

renderer::BufferView ThreeD::getVertexBuffer(u32 vertex_array_index) const {
    const auto& vertex_array = regs.vertex_arrays[vertex_array_index];

    // HACK
    if (static_cast<u64>(vertex_array.addr) == 0x0) {
        ONCE(LOG_ERROR(Engines, "Invalid vertex buffer"));
        return {};
    }

    const auto ptr = tls_crnt_gmmu->unmapAddr(vertex_array.addr);
    const auto size =
        static_cast<u64>(regs.vertex_array_limits[vertex_array_index]) + 1 -
        static_cast<u64>(vertex_array.addr);
    return gpu.getRenderer().getBufferCache().get(
        tls_crnt_command_buffer, ztd::Range<uptr>::fromSize(ptr, size));
}

renderer::ITextureView*
ThreeD::getTexture(const TextureImageControl& tic,
                   std::optional<renderer::TextureType> shader_type,
                   renderer::TextureUsage usage) const {
    // Storage images are written without a swizzle
    const bool is_image = usage == renderer::TextureUsage::Write;
    if (tic.hdr_version == TicHdrVersion::_1DBuffer) {
        // The shader was not specialized for a buffer texture
        if (shader_type && *shader_type != renderer::TextureType::_1D &&
            *shader_type != renderer::TextureType::_1DBuffer) {
            ONCE(LOG_WARN(Engines,
                          "Buffer texture bound to a {} texture binding, "
                          "ignoring",
                          *shader_type));
            return nullptr;
        }

        const auto gpu_addr = makeAddr(tic.addr_lo, tic.addr_hi);
        if (gpu_addr == 0)
            return nullptr;
        const auto format =
            renderer::toTextureFormat(tic.format_word, tic.is_srgb);
        if (format == renderer::TextureFormat::Invalid)
            return nullptr;
        const u64 width =
            (u64(tic.width_minus_one_16_31) << 16) + tic.width_minus_one + 1;
        const u64 bytes = width * renderer::getTextureFormatBpp(format);
        if (width > std::numeric_limits<u32>::max() ||
            bytes > std::numeric_limits<u32>::max()) {
            LOG_ERROR(Engines, "Buffer texture exceeds supported size");
            return nullptr;
        }
        const renderer::TextureDescriptor descriptor(
            tls_crnt_gmmu->unmapAddr(gpu_addr),
            renderer::TextureType::_1DBuffer, format, true,
            static_cast<u32>(bytes), static_cast<u32>(width), 1, 1, 1, 1, 0, 0,
            0);
        const renderer::TextureViewDescriptor view(
            renderer::TextureType::_1DBuffer, format, ztd::Range<u32>(0, 1),
            ztd::Range<u32>(0, 1),
            is_image ? renderer::SwizzleChannels()
                     : renderer::SwizzleChannels(format,
                                                 tic.format_word.swizzle_x,
                                                 tic.format_word.swizzle_y,
                                                 tic.format_word.swizzle_z,
                                                 tic.format_word.swizzle_w));
        return gpu.getRenderer().getTextureCache().find(
            tls_crnt_command_buffer, descriptor, view, usage);
    }

    const uptr gpu_addr = makeAddr(tic.addr_lo, tic.addr_hi);
    if (gpu_addr == 0x0) {
        LOG_ERROR(Engines, "Texture address is NULL");
        return nullptr;
    }

    const auto format = renderer::toTextureFormat(tic.format_word, tic.is_srgb);
    if (format == renderer::TextureFormat::Invalid)
        return nullptr;

    bool is_linear = false;
    u32 linear_stride = 0;
    switch (tic.hdr_version) {
    case TicHdrVersion::Pitch:
        is_linear = true;
        linear_stride = static_cast<u32>(tic.pitch_5_20) << 5u;
        break;
    case TicHdrVersion::BlockLinear:
        break;
    default:
        LOG_NOT_IMPLEMENTED(Engines, "TIC HDR version {}", tic.hdr_version);
        break;
    }

    const auto type = toTextureType(tic.texture_type);

    u32 depth = tic.depth_minus_one + 1;
    u32 layer_count = 1;
    if (type != renderer::TextureType::_3D) {
        layer_count = depth;
        depth = 1;
        if (type == renderer::TextureType::Cube ||
            type == renderer::TextureType::CubeArray)
            layer_count *= 6;
    }

    const u32 level_count = tic.mip_max_levels + 1;
    const renderer::TextureDescriptor descriptor(
        tls_crnt_gmmu->unmapAddr(gpu_addr), type, format, is_linear,
        linear_stride, tic.width_minus_one + 1, tic.height_minus_one + 1, depth,
        level_count, layer_count, tic.sparse_tile_width_gobs_log2,
        tic.tile_height_gobs_log2, tic.tile_depth_gobs_log2);

    // The view type must match the type the shader declares, otherwise Metal
    // binds nothing. Reinterpret between array and non-array types when the
    // shader expects a different one.
    auto view_type = type;
    u32 view_layer_count = layer_count;
    auto target_type = type;
    if (shader_type) {
        target_type = *shader_type;
    } else {
        // Demote array types to non-array types if possible
        if (type == renderer::TextureType::_1DArray && layer_count == 1)
            target_type = renderer::TextureType::_1D;
        else if (type == renderer::TextureType::_2DArray && layer_count == 1)
            target_type = renderer::TextureType::_2D;
        else if (type == renderer::TextureType::CubeArray && layer_count == 6)
            target_type = renderer::TextureType::Cube;
    }
    switch (target_type) {
    case renderer::TextureType::_1D:
        if (type == renderer::TextureType::_1DArray) {
            view_type = target_type;
            view_layer_count = 1;
        }
        break;
    case renderer::TextureType::_1DArray:
        if (type == renderer::TextureType::_1D)
            view_type = target_type;
        break;
    case renderer::TextureType::_2D:
        if (type == renderer::TextureType::_2DArray ||
            type == renderer::TextureType::Cube ||
            type == renderer::TextureType::CubeArray) {
            view_type = target_type;
            view_layer_count = 1;
        }
        break;
    case renderer::TextureType::_2DArray:
        if (type == renderer::TextureType::_2D ||
            type == renderer::TextureType::Cube ||
            type == renderer::TextureType::CubeArray)
            view_type = target_type;
        break;
    case renderer::TextureType::Cube:
        if ((type == renderer::TextureType::CubeArray ||
             type == renderer::TextureType::_2DArray) &&
            layer_count >= 6) {
            view_type = target_type;
            view_layer_count = 6;
        }
        break;
    case renderer::TextureType::CubeArray:
        if ((type == renderer::TextureType::Cube ||
             type == renderer::TextureType::_2DArray) &&
            layer_count >= 6) {
            view_type = target_type;
            view_layer_count = layer_count / 6 * 6;
        }
        break;
    default:
        break;
    }
    if (shader_type && view_type != *shader_type)
        LOG_WARN(Engines, "Texture type mismatch (TIC: {}, shader: {})", type,
                 *shader_type);

    const renderer::TextureViewDescriptor view_descriptor(
        view_type, format, ztd::Range<u32>(0, level_count),
        ztd::Range<u32>(0, view_layer_count),
        is_image ? renderer::SwizzleChannels()
                 : renderer::SwizzleChannels(
                       format, tic.format_word.swizzle_x,
                       tic.format_word.swizzle_y, tic.format_word.swizzle_z,
                       tic.format_word.swizzle_w));

    return gpu.getRenderer().getTextureCache().find(
        tls_crnt_command_buffer, descriptor, view_descriptor, usage);
}

renderer::SamplerBase*
ThreeD::getSampler(const TextureSamplerControl& tsc) const {
    const renderer::SamplerDescriptor descriptor{
        .min_filter = static_cast<renderer::SamplerFilter>(tsc.min_filter),
        .mag_filter = static_cast<renderer::SamplerFilter>(tsc.mag_filter),
        .mip_filter = static_cast<renderer::SamplerMipFilter>(tsc.mip_filter),
        .address_mode_s =
            static_cast<renderer::SamplerAddressMode>(tsc.address_u),
        .address_mode_t =
            static_cast<renderer::SamplerAddressMode>(tsc.address_v),
        .address_mode_r =
            static_cast<renderer::SamplerAddressMode>(tsc.address_p),
        .depth_compare_op = (tsc.depth_compare ? static_cast<CompareOp>(
                                                     tsc.depth_compare_op + 1)
                                               : CompareOp::Invalid),
        .border_color_u = uint4({tsc.border_color_r, tsc.border_color_g,
                                 tsc.border_color_b, tsc.border_color_a}),
    };

    return gpu.getRenderer().getSamplerCache().find(descriptor);
}

void ThreeD::configureShaderStage(
    const ShaderStage stage, const TextureImageControl* tex_header_pool,
    const TextureSamplerControl* tex_sampler_pool) {
    const auto shader_type = toRendererShaderType(stage);
    const u32 stage_index = static_cast<u32>(stage) -
                            1; // 1 is subtracted, because VertexA is skipped

    const auto shader = getShaderUnchecked(stage);
    if (shader == nullptr)
        return;
    const auto& resource_mapping = shader->getDescriptor().resource_mapping;

    // Uniform buffers
    gpu.getRenderer().unbindUniformBuffers(shader_type);
    for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
        const auto index = resource_mapping.uniform_buffers[i];
        if (index == invalid<u32>())
            continue;

        // TODO: analyze the shader to get the max possible size
        const auto range = bound_const_buffers[stage_index][i];
        if (range.getBegin() == 0x0) {
            LOG_WARN(Engines, "Uniform buffer at index {} is not bound", index);
            continue;
        }

        const auto buffer = gpu.getRenderer().getBufferCache().get(
            tls_crnt_command_buffer, range);
        gpu.getRenderer().bindUniformBuffer(buffer, shader_type, index);
    }

    // TODO: storage buffers

    // Textures
    if ((tex_header_pool != nullptr) && (tex_sampler_pool != nullptr)) {
        gpu.getRenderer().unbindTextures(shader_type);
        for (const auto [const_buffer_index, renderer_index] :
             resource_mapping.textures) {
            const auto handle =
                readTextureHandle(stage_index, const_buffer_index);
            if (!handle)
                continue;
            const auto texture_handle = *handle;

            // Image
            const auto image_handle = getImageHandle(texture_handle);
            const auto sampler_handle = getSamplerHandle(texture_handle);
            if ((regs.tex_header_pool_max_index != 0 &&
                 image_handle > regs.tex_header_pool_max_index) ||
                (regs.tex_sampler_pool_max_index != 0 &&
                 sampler_handle > regs.tex_sampler_pool_max_index)) {
                ONCE(LOG_WARN(Engines,
                              "Texture handle {:#010x} (binding {:#x}) is out "
                              "of the pool bounds",
                              texture_handle, const_buffer_index));
                continue;
            }
            const auto& tic = tex_header_pool[image_handle];
            // DEBUG: report descriptors with formats we cannot decode
            if (renderer::toTextureFormat(tic.format_word, tic.is_srgb) ==
                renderer::TextureFormat::Invalid) {
                static u32 bad_tic_count = 0;
                if (bad_tic_count++ < 10) {
                    const auto raw = reinterpret_cast<const u32*>(&tic);
                    LOG_WARN(Engines,
                             "DEBUG bad TIC: stage {}, binding {:#x}, handle "
                             "{:#010x}, image {:#x} (max {:#x}), words {:08x} "
                             "{:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                             stage, const_buffer_index, texture_handle,
                             image_handle, regs.tex_header_pool_max_index,
                             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                             raw[6], raw[7]);
                }
            }

            // DEBUG: inspect descriptors that look like buffer textures
            if (tic.hdr_version == TicHdrVersion::_1DBuffer) {
                static u32 debug_count = 0;
                if (debug_count++ < 20) {
                    const auto raw = reinterpret_cast<const u32*>(&tic);
                    LOG_INFO(Engines,
                             "DEBUG TIC: stage {}, slot {:#x}, cbuf slot {}, "
                             "handle {:#010x}, image {:#x}, max index {:#x}, "
                             "words {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} "
                             "{:08x} {:08x}",
                             stage, const_buffer_index,
                             regs.bindless_texture_const_buffer_slot,
                             texture_handle, image_handle,
                             regs.tex_header_pool_max_index, raw[0], raw[1],
                             raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
                }
            }
            const auto type_it =
                resource_mapping.texture_types.find(const_buffer_index);
            const auto texture =
                getTexture(tic, type_it != resource_mapping.texture_types.end()
                                    ? std::optional(type_it->second)
                                    : std::nullopt);

            // Sampler
            const auto& tsc = tex_sampler_pool[sampler_handle];
            const auto sampler = getSampler(tsc);

            if ((texture != nullptr) && (sampler != nullptr))
                gpu.getRenderer().bindTexture(texture, sampler, shader_type,
                                              renderer_index);
        }
    }
}

void ThreeD::traceDraw(const char* kind, u32 count, u32 first,
                       u32 base_vertex) {
    auto& tracer = FrameTracer::instance();
    const u32 draw_index_for_dump = tracer.nextDrawIndex();
    std::string str = fmt::format(
        "#{} {} count={} first={} base_vertex={} primitive={:#x} "
        "index_type={:#x}\n",
        draw_index_for_dump, kind, count, first, base_vertex,
        static_cast<u32>(regs.begin.primitive_type),
        static_cast<u32>(regs.index_type));

    // Shaders
    for (const auto stage : {ShaderStage::VertexB, ShaderStage::Fragment}) {
        const auto& program = regs.shader_programs[static_cast<usize>(stage)];
        str += fmt::format(
            "    {}: enabled={} shader={} code={:#x}\n",
            stage == ShaderStage::VertexB ? "VS" : "FS",
            static_cast<bool>(program.config.enable),
            fmt::ptr(getShaderUnchecked(stage)),
            static_cast<u64>(regs.shader_program_region) + program.offset);
    }

    // Render targets
    str += fmt::format("    RT count={}",
                       static_cast<u32>(regs.color_target_control.count));
    for (u32 i = 0; i < regs.color_target_control.count; i++) {
        const auto& rt = regs.color_targets[regs.color_target_control.getMap(i)];
        str += fmt::format(" | [{}->{}] addr={:#x} fmt={} {}x{} mask={:#x} "
                           "3d={} layers={} base_layer={} instance={}",
                           i, regs.color_target_control.getMap(i),
                           static_cast<u64>(rt.addr),
                           renderer::toTextureFormat(rt.format),
                           rt.width_or_stride, rt.height,
                           static_cast<u32>(regs.color_write_masks[i]),
                           static_cast<bool>(rt.tile_mode.is_3d),
                           static_cast<u32>(rt.array_mode.layers),
                           rt.base_layer, instance_index);
    }
    str += "\n";
    str += fmt::format(
        "    depth: enabled={} addr={:#x} host={:#x} {}x{} layers={} fmt={} "
        "test={} write={} op={:#x}\n",
        regs.depth_target_enabled != 0u,
        static_cast<u64>(regs.depth_target_addr),
        static_cast<u64>(regs.depth_target_addr) != 0x0
            ? tls_crnt_gmmu->unmapAddr(regs.depth_target_addr)
            : 0x0,
        static_cast<u32>(regs.depth_target_width),
        static_cast<u32>(regs.depth_target_height),
        static_cast<u32>(regs.depth_target_array_mode.layers),
        renderer::toTextureFormat(regs.depth_target_format),
        regs.depth_test_enabled != 0u, regs.depth_write_enabled != 0u,
        static_cast<u32>(regs.depth_compare_op));

    str += fmt::format("    stencil: enabled={} func={:#x} ref={:#x} mask={:#x} "
                       "ops=({:#x}, {:#x}, {:#x}) write={:#x} two_sided={}\n",
                       regs_raw[0x4e0] != 0, regs_raw[0x4e4], regs_raw[0x4e5],
                       regs_raw[0x4e6], regs_raw[0x4e1], regs_raw[0x4e2],
                       regs_raw[0x4e3], regs_raw[0x4e7], regs_raw[0x565] != 0);
    for (u32 i = 0; i < 4; i++) {
        const auto& attrib = regs.vertex_attrib_states[i];
        str += fmt::format(
            "    attrib{}: buffer={} fixed={} offset={} size={:#x} type={} "
            "bgra={}\n",
            i, static_cast<u32>(attrib.buffer_id), attrib.is_fixed,
            static_cast<u32>(attrib.offset), static_cast<u32>(attrib.size),
            static_cast<u32>(attrib.type), attrib.bgra);
    }
    for (u32 i = 0; i < 2; i++) {
        const auto& vertex_array = regs.vertex_arrays[i];
        if (!vertex_array.config.enable ||
            static_cast<u64>(vertex_array.addr) == 0x0)
            continue;
        // The address may not be mapped (yet); don't fault while tracing
        uptr base;
        const auto as = tls_crnt_gmmu->findAddrImplRef(vertex_array.addr, base);
        if (as == nullptr || as->ptr == 0x0)
            continue;
        const auto data = reinterpret_cast<const u8*>(
            as->ptr + (static_cast<uptr>(vertex_array.addr) - base));
        const u32 stride = vertex_array.config.stride;
        const u32 avail = static_cast<u32>(std::min<u64>(
            as->size - (static_cast<uptr>(vertex_array.addr) - base), 48));
        str += fmt::format("    VB{} stride={} bytes: {:02x}\n", i, stride,
                           fmt::join(data, data + std::min(stride, avail), " "));
    }
    str += fmt::format("    cull: enabled={} mode={:#x} front={:#x} origin={:#x}\n",
                       regs.cull_face_enabled != 0,
                       static_cast<u32>(regs.cull_face_mode),
                       static_cast<u32>(regs.front_face_winding),
                       static_cast<u32>(regs.window_origin_flags));
    str += fmt::format("    depth bias: fill={} constant={} slope={} clamp={}\n",
                       regs_raw[0x372] != 0, std::bit_cast<f32>(regs_raw[0x56f]),
                       std::bit_cast<f32>(regs_raw[0x55b]),
                       std::bit_cast<f32>(regs_raw[0x61f]));

    // Blend
    str += fmt::format("    blend: independent={} const=({}, {}, {}, {})",
                       regs.independent_blend_enabled != 0u,
                       regs.blend_constant.x(), regs.blend_constant.y(),
                       regs.blend_constant.z(), regs.blend_constant.w());
    for (u32 i = 0; i < regs.color_target_control.count; i++) {
        if (regs.color_blend_enabled[i] == 0u) {
            str += fmt::format(" | [{}] off", i);
            continue;
        }
        if (regs.independent_blend_enabled != 0u) {
            const auto& b = regs.independent_blend_state[i];
            str += fmt::format(" | [{}] rgb {:#x}({:#x},{:#x}) a {:#x}({:#x},{:#x})",
                               i, b.rgb_op, b.src_rgb_factor, b.dst_rgb_factor,
                               b.alpha_op, b.src_alpha_factor,
                               b.dst_alpha_factor);
        } else {
            const auto& b = regs.blend_state;
            str += fmt::format(" | [{}] rgb {:#x}({:#x},{:#x}) a {:#x}({:#x},{:#x})",
                               i, b.rgb_op, b.src_rgb_factor, b.dst_rgb_factor,
                               b.alpha_op, b.src_alpha_factor,
                               b.dst_alpha_factor);
        }
    }
    str += "\n";

    // Viewport and scissor
    const auto viewport = getViewport(0);
    const auto scissor = getScissor(0);
    str += fmt::format("    viewport=({}, {}, {}x{}, z {}..{}) scissor=({}, {}, "
                       "{}x{})\n",
                       viewport.rect.origin.x(), viewport.rect.origin.y(),
                       viewport.rect.size.x(), viewport.rect.size.y(),
                       viewport.depth_near, viewport.depth_far,
                       scissor.origin.x(), scissor.origin.y(),
                       scissor.size.x(), scissor.size.y());

    // Uniform buffers used by the shaders
    for (const auto stage : {ShaderStage::VertexB, ShaderStage::Fragment}) {
        const auto shader = getShaderUnchecked(stage);
        if (shader == nullptr)
            continue;
        const u32 stage_index = static_cast<u32>(stage) - 1;
        const auto& mapping = shader->getDescriptor().resource_mapping;
        for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
            if (mapping.uniform_buffers[i] == invalid<u32>() ||
                i == regs.bindless_texture_const_buffer_slot)
                continue;
            const auto range = bound_const_buffers[stage_index][i];
            if (range.getBegin() == 0x0) {
                str += fmt::format("    {} c{}: NOT BOUND\n",
                                   stage == ShaderStage::VertexB ? "VS" : "FS",
                                   i);
                continue;
            }
            const auto data = reinterpret_cast<const f32*>(range.getBegin());
            const usize n = std::min<usize>(range.getSize() / sizeof(f32), 40);
            str += fmt::format("    {} c{} (size {:#x}): {}\n",
                               stage == ShaderStage::VertexB ? "VS" : "FS", i,
                               range.getSize(), fmt::join(data, data + n, " "));
        }
    }

    // Textures
    const auto pool_addr = static_cast<u64>(regs.tex_header_pool);
    const auto* pool =
        pool_addr != 0x0 ? reinterpret_cast<const TextureImageControl*>(
                               tls_crnt_gmmu->unmapAddr(pool_addr))
                         : nullptr;
    for (const auto stage : {ShaderStage::VertexB, ShaderStage::Fragment}) {
        const auto shader = getShaderUnchecked(stage);
        if (shader == nullptr || pool == nullptr)
            continue;
        const u32 stage_index = static_cast<u32>(stage) - 1;
        const auto& mapping = shader->getDescriptor().resource_mapping;
        for (const auto [index, renderer_index] : mapping.textures) {
            const auto handle_opt = readTextureHandle(stage_index, index);
            if (!handle_opt)
                continue;
            const auto handle = *handle_opt;
            const auto& tic = pool[getImageHandle(handle)];
            const auto type_it = mapping.texture_types.find(index);
            str += fmt::format(
                "    {} tex slot {:#x} (shader type {}): handle={:#010x} ",
                stage == ShaderStage::VertexB ? "VS" : "FS", index,
                type_it != mapping.texture_types.end()
                    ? fmt::format("{}", type_it->second)
                    : "?",
                handle);
            if (makeAddr(tic.addr_lo, tic.addr_hi) == 0x0) {
                str += "EMPTY\n";
                continue;
            }
            {
                const auto tsc_pool_addr = static_cast<u64>(regs.tex_sampler_pool);
                if (tsc_pool_addr != 0x0) {
                    const auto* tsc_pool =
                        reinterpret_cast<const TextureSamplerControl*>(
                            tls_crnt_gmmu->unmapAddr(tsc_pool_addr));
                    const auto& tsc = tsc_pool[getSamplerHandle(handle)];
                    str += fmt::format(
                        "[tsc min={} mag={} mip={} compare={} op={}] ",
                        static_cast<u32>(tsc.min_filter),
                        static_cast<u32>(tsc.mag_filter),
                        static_cast<u32>(tsc.mip_filter),
                        static_cast<u32>(tsc.depth_compare),
                        static_cast<u32>(tsc.depth_compare_op));
                }
            }
            str += fmt::format(
                "hdr={} type={} fmt={} swz={}{}{}{} {}x{} depth={} levels={} "
                "addr={:#x} host={:#x}\n",
                static_cast<u32>(tic.hdr_version),
                toTextureType(tic.texture_type),
                renderer::toTextureFormat(tic.format_word, tic.is_srgb),
                static_cast<u32>(tic.format_word.swizzle_x),
                static_cast<u32>(tic.format_word.swizzle_y),
                static_cast<u32>(tic.format_word.swizzle_z),
                static_cast<u32>(tic.format_word.swizzle_w),
                tic.width_minus_one + 1, tic.height_minus_one + 1,
                tic.depth_minus_one + 1, tic.mip_max_levels + 1,
                makeAddr(tic.addr_lo, tic.addr_hi), uptr{0});

            // Inspect the guest memory of the texture (approximate size).
            // Descriptors may be garbage, so stay within the mapping.
            const auto format =
                renderer::toTextureFormat(tic.format_word, tic.is_srgb);
            uptr tex_base;
            const auto tex_as = tls_crnt_gmmu->findAddrImplRef(
                makeAddr(tic.addr_lo, tic.addr_hi), tex_base);
            if (format != renderer::TextureFormat::Invalid &&
                tex_as != nullptr && tex_as->ptr != 0x0) {
                const uptr offset = makeAddr(tic.addr_lo, tic.addr_hi) - tex_base;
                const usize size = std::min<usize>(
                    static_cast<usize>(renderer::getTextureFormatStride(
                        format, tic.width_minus_one + 1u)) *
                        renderer::getTextureFormatRows(
                            format, tic.height_minus_one + 1u),
                    tex_as->size - offset);
                const auto data =
                    reinterpret_cast<const u8*>(tex_as->ptr + offset);
                usize non_zero = 0;
                for (usize i = 0; i < size; i++)
                    non_zero += (data[i] != 0);
                str += fmt::format("        memory: size={:#x} non_zero={} "
                                   "first16={:02x}\n",
                                   size, non_zero,
                                   fmt::join(data, data + std::min<usize>(size, 16), " "));
            }
        }
    }

    tracer.write(str);

    // DEBUG: dump the first color target before the draw (HYDRA_TRACE_DUMP_RT)
    static const bool dump_rt = std::getenv("HYDRA_TRACE_DUMP_RT") != nullptr;
    if (dump_rt && regs.color_target_control.count > 0) {
        const auto rt_index = regs.color_target_control.getMap(0);
        const auto& rt = regs.color_targets[rt_index];
        if (static_cast<u64>(rt.addr) != 0x0 && rt.height >= 400) {
            std::scoped_lock texture_cache_lock(
                gpu.getRenderer().getTextureCache().getMutex());
            if (const auto view = getColorTargetTexture(rt_index)) {
                const auto path =
                    tracer.getDir() / fmt::format("f{}_d{:03}_rt0.png",
                                                  tracer.getFrameIndex(),
                                                  draw_index_for_dump);
                gpu.getRenderer().debugDumpTexture(tls_crnt_command_buffer,
                                                   view, path.string());
            }
        }
    }

    // Dump the textures read by the fragment shader
    if (tracer.isDumpEnabled() && pool != nullptr) {
        const auto shader = getShaderUnchecked(ShaderStage::Fragment);
        const u32 stage_index = static_cast<u32>(ShaderStage::Fragment) - 1;
        if (shader != nullptr) {
            std::scoped_lock texture_cache_lock(
                gpu.getRenderer().getTextureCache().getMutex());
            const auto& mapping = shader->getDescriptor().resource_mapping;
            for (const auto [index, renderer_index] : mapping.textures) {
                const auto handle = readTextureHandle(stage_index, index);
                if (!handle)
                    continue;
                const auto& tic = pool[getImageHandle(*handle)];
                if (makeAddr(tic.addr_lo, tic.addr_hi) == 0x0 ||
                    tic.hdr_version == TicHdrVersion::_1DBuffer)
                    continue;
                const auto type_it = mapping.texture_types.find(index);
                const auto view = getTexture(
                    tic, type_it != mapping.texture_types.end()
                             ? std::optional(type_it->second)
                             : std::nullopt);
                if (view == nullptr || !tracer.markDumped(view))
                    continue;
                const auto path =
                    tracer.getDir() /
                    fmt::format("f{}_d{:03}_slot{:x}_{}_{}x{}.png",
                                tracer.getFrameIndex(), draw_index_for_dump,
                                index,
                                renderer::toTextureFormat(tic.format_word,
                                                          tic.is_srgb),
                                tic.width_minus_one + 1,
                                tic.height_minus_one + 1);
                gpu.getRenderer().debugDumpTexture(tls_crnt_command_buffer,
                                                   view, path.string());
            }
        }
    }
}

std::optional<u32> ThreeD::readTextureHandle(u32 stage_index,
                                             u32 binding_index) const {
    u32 slot = regs.bindless_texture_const_buffer_slot;
    u32 word = binding_index;
    u32 sampler_word = renderer::TRACKED_TEXTURE_NO_SAMPLER;
    if (renderer::isTrackedTextureIndex(binding_index)) {
        slot = renderer::getTrackedTextureSlot(binding_index);
        word = renderer::getTrackedTextureWord(binding_index);
        sampler_word = renderer::getTrackedSamplerWord(binding_index);
    }
    if (slot >= CONST_BUFFER_BINDING_COUNT)
        return std::nullopt;

    const auto& range = bound_const_buffers[stage_index][slot];
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

bool ThreeD::drawInternal() {
    PerfStats::instance().draws++;

    std::scoped_lock texture_cache_lock(
        gpu.getRenderer().getTextureCache().getMutex());

    // Flush tracked pages
    tls_crnt_gmmu->getMmu()->flushTrackedPages();

    // State
    if (!regs.shader_programs[static_cast<u32>(ShaderStage::VertexB)]
             .config.enable) {
        LOG_WARN(Engines, "Vertex B stage not enabled, skipping draw");
        return false;
    }

    gpu.getRenderer().bindRenderPass(getRenderPass());
    gpu.getRenderer().bindPipeline(getPipeline());

    static const bool highlight_keep_depth =
        std::getenv("HYDRA_HIGHLIGHT_KEEP_DEPTH") != nullptr;
    gpu.getRenderer().setDepthTestEnabled(
        regs.depth_test_enabled != 0u &&
        (highlight_keep_depth || !isHighlightedDraw()));
    gpu.getRenderer().setDepthWriteEnabled(regs.depth_write_enabled != 0u);
    gpu.getRenderer().setDepthCompareOp(regs.depth_compare_op);
    gpu.getRenderer().setBlendConstant(regs.blend_constant);

    // Stencil, register offsets from NVIDIA's clb197.h
    {
        const auto decode_op = [](u32 value) {
            switch (value) {
            case 0x1e00: // OGL_KEEP
            case 1:      // D3D_KEEP
                return renderer::StencilOp::Keep;
            case 0x0: // OGL_ZERO
            case 2:   // D3D_ZERO
                return renderer::StencilOp::Zero;
            case 0x1e01: // OGL_REPLACE
            case 3:      // D3D_REPLACE
                return renderer::StencilOp::Replace;
            case 0x1e02: // OGL_INCRSAT
            case 4:      // D3D_INCRSAT
                return renderer::StencilOp::IncrSat;
            case 0x1e03: // OGL_DECRSAT
            case 5:      // D3D_DECRSAT
                return renderer::StencilOp::DecrSat;
            case 0x150a: // OGL_INVERT
            case 6:      // D3D_INVERT
                return renderer::StencilOp::Invert;
            case 0x8507: // OGL_INCR
            case 7:      // D3D_INCR
                return renderer::StencilOp::Incr;
            case 0x8508: // OGL_DECR
            case 8:      // D3D_DECR
                return renderer::StencilOp::Decr;
            default:
                return renderer::StencilOp::Keep;
            }
        };
        const auto decode_func = [](u32 value) {
            if (value >= 0x200 && value <= 0x207) // OGL
                return static_cast<CompareOp>(value - 0x200 + 1);
            if (value >= 1 && value <= 8) // D3D
                return static_cast<CompareOp>(value);
            return CompareOp::Always;
        };

        renderer::StencilState stencil{};
        stencil.enabled = regs_raw[0x4e0] != 0;
        stencil.front = {
            .func = decode_func(regs_raw[0x4e4]),
            .fail_op = decode_op(regs_raw[0x4e1]),
            .depth_fail_op = decode_op(regs_raw[0x4e2]),
            .pass_op = decode_op(regs_raw[0x4e3]),
            .ref = regs_raw[0x4e5],
            .func_mask = regs_raw[0x4e6],
            .write_mask = regs_raw[0x4e7],
        };
        if (regs_raw[0x565] != 0) { // Two sided
            stencil.back = {
                .func = decode_func(regs_raw[0x569]),
                .fail_op = decode_op(regs_raw[0x566]),
                .depth_fail_op = decode_op(regs_raw[0x567]),
                .pass_op = decode_op(regs_raw[0x568]),
                .ref = regs_raw[0x3d5],
                .func_mask = regs_raw[0x3d7],
                .write_mask = regs_raw[0x3d6],
            };
        } else {
            stencil.back = stencil.front;
        }
        gpu.getRenderer().setStencilState(stencil);
    }

    // Culling. Metal determines the winding in the same window space as the
    // guest, but FLIP_Y inverts the front face.
    auto front_face = regs.front_face_winding;
    if (any(regs.window_origin_flags & WindowOriginFlags::FlipY))
        front_face = (front_face == Winding::Clockwise)
                         ? Winding::CounterClockwise
                         : Winding::Clockwise;
    gpu.getRenderer().setCullState(regs.cull_face_enabled != 0,
                                   regs.cull_face_mode, front_face);

    // Depth bias (polygon offset), register offsets from NVIDIA's clb197.h
    constexpr u32 REG_POLY_OFFSET_FILL = 0x372;
    constexpr u32 REG_SLOPE_SCALE_DEPTH_BIAS = 0x55b;
    constexpr u32 REG_DEPTH_BIAS = 0x56f;
    constexpr u32 REG_DEPTH_BIAS_CLAMP = 0x61f;
    if (regs_raw[REG_POLY_OFFSET_FILL] != 0) {
        gpu.getRenderer().setDepthBias(
            std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS]),
            std::bit_cast<f32>(regs_raw[REG_SLOPE_SCALE_DEPTH_BIAS]),
            std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS_CLAMP]));
    } else {
        gpu.getRenderer().setDepthBias(0.0f, 0.0f, 0.0f);
    }

    for (u32 i = 0; i < VIEWPORT_COUNT; i++) {
        gpu.getRenderer().setViewport(i, getViewport(i));
        gpu.getRenderer().setScissor(i, getScissor(i));
    }

    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++) {
        const auto& vertex_array = regs.vertex_arrays[i];
        // HACK: Super Meat Boy contains invalid vertex arrays with address 4096
        if (!vertex_array.config.enable ||
            (vertex_array.addr.hi == 0 && vertex_array.addr.lo == 4096)) {
            gpu.getRenderer().bindVertexBuffer(renderer::BufferView(), i);
            continue;
        }

        const auto buffer = getVertexBuffer(i);
        gpu.getRenderer().bindVertexBuffer(buffer, i);
    }

    // Configure stages
    const auto tex_header_pool_gpu_addr =
        static_cast<u64>(regs.tex_header_pool);
    const auto tex_sampler_pool_gpu_addr =
        static_cast<u64>(regs.tex_sampler_pool);
    const auto tex_header_pool =
        tex_header_pool_gpu_addr != 0x0
            ? reinterpret_cast<TextureImageControl*>(
                  tls_crnt_gmmu->unmapAddr(tex_header_pool_gpu_addr))
            : nullptr;
    const auto tex_sampler_pool =
        tex_sampler_pool_gpu_addr != 0x0
            ? reinterpret_cast<TextureSamplerControl*>(
                  tls_crnt_gmmu->unmapAddr(tex_sampler_pool_gpu_addr))
            : nullptr;

    // TODO: configure all stages
    configureShaderStage(ShaderStage::VertexB, tex_header_pool,
                         tex_sampler_pool);
    configureShaderStage(ShaderStage::Fragment, tex_header_pool,
                         tex_sampler_pool);

    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::engines

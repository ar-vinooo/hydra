#include "core/hw/tegra_x1/gpu/engines/3d.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/engines/storage_buffer.hpp"
#include "core/hw/tegra_x1/gpu/frame_tracer.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#include "core/hot/table.hpp"
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
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

namespace {

macro::DriverBase* createMacroDriver(ThreeD& three_d) {
    // TODO: choose based on Macro backend
    return new macro::interpreter::Driver(three_d);
}

u32 getImageHandle(u32 handle) { return extractBits(handle, 0, 20); }

// Points 0, lines 1, polygons 2
u32 getPrimitiveClass(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::Points:
        return 0;
    case PrimitiveType::Lines:
    case PrimitiveType::LineLoop:
    case PrimitiveType::LineStrip:
    case PrimitiveType::LinesAdjacency:
    case PrimitiveType::LineStripAdjacency:
        return 1;
    default:
        return 2;
    }
}

// Compare functions use either the D3D (1-8) or the OpenGL (0x200-0x207)
// encoding
CompareOp decodeCompareOp(u32 value) {
    if (value >= 0x200 && value <= 0x207)
        return static_cast<CompareOp>(value - 0x200 + 1);
    if (value >= 1 && value <= 8)
        return static_cast<CompareOp>(value);
    return CompareOp::Always;
}
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
    default:
        ONCE(LOG_WARN(Engines, "Invalid TIC texture type {}",
                      static_cast<u32>(type)));
        return renderer::TextureType::_2D;
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
        case 0x8001: return renderer::BlendFactor::ConstColor;
        case 0x8002: return renderer::BlendFactor::InvConstColor;
        case 0x8003: return renderer::BlendFactor::ConstAlpha;
        case 0x8004: return renderer::BlendFactor::InvConstAlpha;
        case 0x8900: return renderer::BlendFactor::Src1Color;
        case 0x8901: return renderer::BlendFactor::InvSrc1Color;
        case 0x8902: return renderer::BlendFactor::Src1Alpha;
        case 0x8903: return renderer::BlendFactor::InvSrc1Alpha;
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
        case 12: return renderer::BlendFactor::ConstAlpha;
        case 13: return renderer::BlendFactor::InvConstAlpha;
        case 14: return renderer::BlendFactor::ConstColor;
        case 15: return renderer::BlendFactor::InvConstColor;
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

DEFINE_METHOD_TABLE(ThreeD, INLINE_ENGINE_TABLE, 0x485, 2,
                    drawVertexArrayCompact, u32, 0x5f9, 6,
                    drawVertexElementsCompact, u32, 0x45, 1,
                    loadMmeInstructionRamPointer, u32, 0x46, 1,
                    loadMmeInstructionRam, u32, 0x47, 1,
                    loadMmeStartAddressRamPointer, u32, 0x48, 1,
                    loadMmeStartAddressRam, u32, 0x35e, 1, drawVertexArray, u32,
                    0x54c, 1, resetCounter, u32, 0x586, 1, beginDraw, u32,
                    0x585, 1, endDraw, u32, 0x57a, 1, inlineIndex32, u32,
                    0x57c, 1, inlineIndex16, u32, 0x4c1, 1, inlineIndex8, u32,
                    0x5f8, 1, drawVertexElements, u32, 0x674, 1, clearBuffer,
                    ClearBufferData, 0x6c3, 1, setReportSemaphore, u32, 0x8c4,
                    1, firmwareCall4, u32, 0x8e4, 16, loadConstBuffer, u32,
                    0x900, 5 * 8, bindGroup, u32)

ThreeD::ThreeD(Gpu& gpu_) : gpu{gpu_}, macro_driver{createMacroDriver(*this)} {
    // Initialize default state

    // Rasterization and rendering are on at power on
    regs_raw[0xdf] = 1;
    regs_raw[0x556] = 1;

    const auto reset_blend = [](auto& blend) {
        blend.rgb_op = blend.alpha_op = 1;
        blend.src_rgb_factor = blend.src_alpha_factor = 2;
        blend.dst_rgb_factor = blend.dst_alpha_factor = 1;
    };
    reset_blend(regs.blend_state);
    for (auto& blend : regs.independent_blend_state)
        reset_blend(blend);

    // Viewports
    // The depth range is not always set, hardware expects 0..1 (Eden does the
    // same, for ARMS)
    for (auto& viewport : regs.viewports) {
        viewport.near = 0.0f;
        viewport.far = 1.0f;
    }
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

void ThreeD::flushMacroHot() { macro_driver->execute(); }

void ThreeD::macro(u32 method, u32 arg) {
    LOG_DEBUG(Engines, "Macro method {:#x} argument {:#010x}", method, arg);
    macro_driver->pushArgument(method - MACRO_METHODS_REGION, arg);
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

// NVN titles (UE4 included) leave SET_VIEWPORT_SCALE_OFFSET at 0 while they
// draw clip space vertices with viewport transforms, so the register cannot
// select the window space path: the transform always applies. Treating 0 as
// "window space" collapsed every UE4 draw into one pixel.
static bool viewportTransformEnabled() { return true; }

bool ThreeD::shouldExecute() const {
    // SET_RENDER_ENABLE_C (mode) and the render enable override
    constexpr u32 REG_RENDER_ENABLE_MODE = 0x556;
    constexpr u32 REG_RENDER_ENABLE_OVERRIDE = 0x651;
    switch (regs_raw[REG_RENDER_ENABLE_OVERRIDE] & 0x3) {
    case 1:
        return true;
    case 2:
        return false;
    default:
        break;
    }
    const auto mode = regs_raw[REG_RENDER_ENABLE_MODE] & 0x7;
    if (mode == 0)
        return false;
    if (mode == 1)
        return true;
    if (mode > 4) {
        ONCE(LOG_NOT_IMPLEMENTED(Engines, "Conditional rendering mode {}", mode));
        return true;
    }

    // Reports have already published completion-ordered guest data.
    const u64 address = (static_cast<u64>(regs_raw[0x554] & 0xff) << 32) |
                        regs_raw[0x555];
    std::array<u32, 6> compare{};
    auto* data = reinterpret_cast<u8*>(compare.data());
    usize offset = 0;
    while (offset < sizeof(compare)) {
        uptr base;
        const auto as = tls_crnt_gmmu != nullptr
                            ? tls_crnt_gmmu->findAddr(address + offset, base)
                            : std::nullopt;
        if (!as || as->ptr == 0) {
            ONCE(LOG_WARN(Engines, "Unmapped conditional rendering data {:#x}", address));
            return false;
        }
        const auto bytes = std::min<u64>(sizeof(compare) - offset,
                                          as->size - (address + offset - base));
        std::memcpy(data + offset,
                    reinterpret_cast<const void*>(as->ptr + address + offset - base),
                    bytes);
        offset += bytes;
    }
    switch (mode) {
    case 2:
        return compare[0] != 0 && compare[1] != 0;
    case 3:
        return compare[0] == compare[4] && compare[1] == compare[5];
    default:
        return compare[0] != compare[4] || compare[1] != compare[5];
    }
}

void ThreeD::drawVertexArrayCompact(const u32 index, const u32 arg) {
    // 0 first, 1 subsequent
    if (index == 0)
        instance_index = 0;
    else
        instance_index++;

    const auto start = regs.vertex_array_start;
    const auto primitive_type = regs.begin.primitive_type;
    regs.vertex_array_start = extractBits(arg, 0, 16);
    regs.begin.primitive_type =
        static_cast<PrimitiveType>(extractBits(arg, 28, 3));
    drawVertexArray(0, extractBits(arg, 16, 12));
    regs.vertex_array_start = start;
    regs.begin.primitive_type = primitive_type;
}

void ThreeD::drawVertexElementsCompact(const u32 index, const u32 arg) {
    // 32, 16 and 8-bit indices, first then subsequent instances
    constexpr IndexType formats[] = {IndexType::UInt32, IndexType::UInt16,
                                     IndexType::UInt8};
    if (index < 3)
        instance_index = 0;
    else
        instance_index++;

    const auto first = regs.vertex_elements_start;
    const auto index_type = regs.index_type;
    const auto primitive_type = regs.begin.primitive_type;
    regs.vertex_elements_start = extractBits(arg, 0, 16);
    regs.index_type = formats[index % 3];
    regs.begin.primitive_type =
        static_cast<PrimitiveType>(extractBits(arg, 28, 4));
    drawVertexElements(0, extractBits(arg, 16, 12));
    regs.vertex_elements_start = first;
    regs.index_type = index_type;
    regs.begin.primitive_type = primitive_type;
}

void ThreeD::beginDraw(const u32 index, const u32 arg) {
    writeReg(0x586, arg);
    inline_indices.clear();

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

void ThreeD::inlineIndex32(const u32, const u32 arg) {
    inline_indices.push_back(arg);
}

void ThreeD::inlineIndex16(const u32, const u32 arg) {
    inline_indices.push_back(arg & 0xffff);
    inline_indices.push_back(arg >> 16);
}

void ThreeD::inlineIndex8(const u32, const u32 arg) {
    for (u32 shift = 0; shift < 32; shift += 8)
        inline_indices.push_back((arg >> shift) & 0xff);
}

void ThreeD::endDraw(const u32, const u32) {
    if (inline_indices.empty())
        return;
    drawVertexElementsImpl(
        static_cast<u32>(inline_indices.size()),
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(inline_indices.data()),
                                   inline_indices.size() * sizeof(u32)));
    inline_indices.clear();
}

void ThreeD::drawVertexArray(const u32 index, u32 count) {
    auto& visual = VisualDiagnostics::instance();
    struct DrawEnd {
        VisualDiagnostics& visual;
        ~DrawEnd() { visual.endDraw(); }
    } draw_end{visual};
    auto index_type = IndexType::None;
    auto primitive_type = regs.begin.primitive_type;
    renderer::BufferView index_buffer;
    {
        std::scoped_lock buffer_cache_lock(
            gpu.getRenderer().getBufferCache().getMutex());
        if (!drawInternal())
            return;
        captureVisualDraw(false);
        if (visual.skipDraw()) {
            visual.event("draw_skipped");
            return;
        }
        if (FrameTracer::instance().isActive())
            traceDraw("draw", count, regs.vertex_array_start, 0);

        index_buffer = gpu.getRenderer().getIndexCache().decode(
            tls_crnt_command_buffer,
            {.type = index_type,
             .primitive_type = primitive_type,
             .count = count},
            index_type, primitive_type, count);
    }
    // Decoding can leave nothing to draw (e.g. a fan of 2 vertices)
    if (count == 0)
        return;

    if (index_buffer.getBase() != nullptr) {
        // Bind index buffer
        gpu.getRenderer().bindIndexBuffer(index_buffer, index_type);

        // Draw

        // Vertex start is set as vertex base instead, as start is now index
        // start
        // TODO: instance count
        labelCaptureDraw(count);
        gpu.getRenderer().drawIndexed(tls_crnt_command_buffer, primitive_type,
                                      0, count, regs.vertex_array_start,
                                      instance_index, 1);
        profileDraw(count);
        captureVisualDraw(true);
    } else {
        // Draw
        // TODO: instance count
        labelCaptureDraw(count);
        gpu.getRenderer().draw(tls_crnt_command_buffer, primitive_type,
                               regs.vertex_array_start, count, instance_index,
                               1);
        profileDraw(count);
        captureVisualDraw(true);
    }
}

// Names each draw in a GPU capture after its guest shaders, so Metal draws can
// be matched with Hydra's shader addresses and traces
void ThreeD::labelCaptureDraw(u32 count) {
    auto& renderer = gpu.getRenderer();
    if (!renderer.isCapturing()) {
        capture_draw_index = 0;
        return;
    }
    const auto program_addr = [&](ShaderStage stage) {
        return static_cast<u64>(regs.shader_program_region) +
               regs.shader_programs[static_cast<usize>(stage)].offset;
    };
    renderer.setCaptureDrawLabel(fmt::format(
        "d{} vs {:#x} fs {:#x} n={} rt0={:#x}", capture_draw_index++,
        program_addr(ShaderStage::VertexB), program_addr(ShaderStage::Fragment),
        count, static_cast<u64>(regs.color_targets[0].addr)));
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

void ThreeD::drawVertexElements(const u32, u32 count) {
    drawVertexElementsImpl(count, std::nullopt);
}

void ThreeD::drawVertexElementsImpl(
    u32 count, std::optional<ztd::Range<uptr>> inline_range) {
    auto& visual = VisualDiagnostics::instance();
    struct DrawEnd {
        VisualDiagnostics& visual;
        ~DrawEnd() { visual.endDraw(); }
    } draw_end{visual};
    auto index_type = inline_range ? IndexType::UInt32 : regs.index_type;
    auto primitive_type = regs.begin.primitive_type;
    renderer::BufferView index_buffer;
    {
        std::scoped_lock buffer_cache_lock(
            gpu.getRenderer().getBufferCache().getMutex());
        if (!drawInternal())
            return;
        captureVisualDraw(false);
        if (visual.skipDraw()) {
            visual.event("draw_skipped");
            return;
        }
        if (FrameTracer::instance().isActive())
            traceDraw("drawIndexed", count, regs.vertex_elements_start,
                      regs.base_vertex);

        // Index buffer: the draw starts at index FIRST and reads no further
        // than the limit (the address of the last valid byte)
        auto range = inline_range.value_or(ztd::Range<uptr>{});
        if (!inline_range) {
            const u64 index_size = getIndexTypeSize(regs.index_type);
            const u64 first_offset =
                static_cast<u64>(regs.vertex_elements_start) * index_size;
            const auto index_buffer_addr = static_cast<u64>(regs.index_buffer_addr);
            const auto index_buffer_limit =
                static_cast<u64>(regs.index_buffer_limit_addr);
            if (index_buffer_limit >= index_buffer_addr) {
                const u64 available = index_buffer_limit - index_buffer_addr + 1;
                const u64 readable =
                    first_offset < available ? available - first_offset : 0;
                if (static_cast<u64>(count) * index_size > readable) {
                    ONCE(LOG_WARN(Engines,
                                  "Indexed draw reads past its index buffer "
                                  "(first: {}, count: {})",
                                  regs.vertex_elements_start, count));
                    count = static_cast<u32>(readable / index_size);
                }
            }
            if (count == 0)
                return;
            range = ztd::Range<uptr>::fromSize(
                tls_crnt_gmmu->unmapAddr(index_buffer_addr + first_offset),
                static_cast<u64>(count) * index_size);
        }

        index_buffer = gpu.getRenderer().getIndexCache().decode(
            tls_crnt_command_buffer,
            {.type = index_type,
             .primitive_type = primitive_type,
             .count = count,
             .mem_range = range,
             .restart_enabled = (regs.primitive_restart_enabled & 1) != 0,
             .restart_index = regs.primitive_restart_index},
            index_type, primitive_type, count);
    }
    if (count == 0)
        return;

    // Bind index buffer
    ASSERT_DEBUG(index_buffer.getBase(), Gpu, "Index buffer not found");
    gpu.getRenderer().bindIndexBuffer(index_buffer, index_type);

    // Draw (the index buffer view starts at the first index)
    // TODO: instance count
    labelCaptureDraw(count);
    gpu.getRenderer().drawIndexed(tls_crnt_command_buffer, primitive_type, 0,
                                  count, regs.base_vertex, instance_index, 1);
    profileDraw(count);
    captureVisualDraw(true);
}

void ThreeD::clearBuffer(const u32 index, const ClearBufferData data) {
    if (!shouldExecute())
        return;

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

    // The stencil write mask applies when the clear control asks for it
    constexpr u32 REG_CLEAR_CONTROL = 0x43e;
    constexpr u32 REG_STENCIL_FRONT_MASK = 0x4e7;
    const bool respect_stencil_mask = (regs_raw[REG_CLEAR_CONTROL] & 0x1) != 0;
    gpu.getRenderer().setClearState({
        .rect = getClearRect(),
        .stencil_write_mask =
            respect_stencil_mask ? regs_raw[REG_STENCIL_FRONT_MASK] & 0xff : 0xff,
    });

    if (data.color_mask != 0x0 && data.target_id < COLOR_TARGET_COUNT)
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

void ThreeD::resetCounter(const u32 index, const u32 data) {
    (void)index;
    // 0x1: ZPassPixelCount
    if (data == 0x1)
        gpu.getRenderer().resetOcclusionCounter();
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

    const u64 semaphore_address = (static_cast<u64>(regs_raw[0x6c0] & 0xff) << 32) |
                                  regs_raw[0x6c1];
    // Reduction (bits 3, 9-11 and 17-18) combines the payload into the word
    // in memory instead of replacing it
    const bool reduction = operation == 0 && extractBits(data, 3, 1) != 0;

    u64 value;
    switch (operation) {
    case 0: // Release
        value = regs.report_semaphore_payload;
        break;
    case 1: { // Acquire
        // Wait until memory equals (or is at least) the payload. Another
        // channel or the CPU releases it. The wait is bounded so that a
        // semaphore nobody releases cannot hang the whole GPU.
        uptr base;
        const auto as = tls_crnt_gmmu->findAddr(semaphore_address, base);
        if (!as || as->ptr == 0) {
            ONCE(LOG_WARN(Engines, "Acquire of unmapped GPU address {:#x}",
                          semaphore_address));
            return;
        }
        auto* word = reinterpret_cast<u32*>(as->ptr + semaphore_address - base);
        const u32 payload = regs.report_semaphore_payload;
        const bool greater_equal = extractBits(data, 16, 1) != 0;
        const auto start = std::chrono::steady_clock::now();
        while (true) {
            const u32 current = std::atomic_ref<u32>(*word).load(
                std::memory_order_acquire);
            if (greater_equal ? static_cast<i32>(current - payload) >= 0
                              : current == payload)
                return;
            if (std::chrono::steady_clock::now() - start >
                std::chrono::seconds(1)) {
                ONCE(LOG_WARN(Engines,
                              "Semaphore acquire at {:#x} timed out ({:#x} "
                              "vs {:#x})",
                              semaphore_address, current, payload));
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    case 2: // Counter
        switch (counter_type) {
        case 0x0: // Payload
            value = regs.report_semaphore_payload;
            break;
        case 0x2:  // ZPassPixelCount
        case 0x15: // ZPassPixelCount64
            if (const auto count = gpu.getRenderer().getOcclusionCounter(
                    tls_crnt_command_buffer))
                value = *count;
            else
                return;
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

    // Counters already synchronized while reading their visibility buffers.
    const bool counter = operation == 2 &&
                         (counter_type == 0x2 || counter_type == 0x15);
    if (!counter && !gpu.getRenderer().synchronize(tls_crnt_command_buffer))
        return;

    struct Span {
        u8* ptr;
        usize offset;
        usize size;
    };
    std::array<Span, 16> spans{};
    usize span_count = 0;
    const usize size = short_query ? 4 : 16;
    const u64 address = semaphore_address;
    usize offset = 0;
    while (offset < size) {
        uptr base;
        const auto as = tls_crnt_gmmu->findAddr(address + offset, base);
        if (!as || as->ptr == 0) {
            ONCE(LOG_WARN(Engines, "Report to unmapped GPU address {:#x}", address));
            return;
        }
        const auto bytes = std::min<u64>(size - offset,
                                          as->size - (address + offset - base));
        spans[span_count++] = {
            reinterpret_cast<u8*>(as->ptr + address + offset - base),
            offset, static_cast<usize>(bytes)};
        offset += bytes;
    }
    {
        auto& cache = gpu.getRenderer().getTextureCache();
        std::scoped_lock lock(cache.getMutex());
        for (usize i = 0; i < span_count; i++)
            cache.flushMemory(tls_crnt_command_buffer,
                              ztd::Range<uptr>::fromSize(
                                  reinterpret_cast<uptr>(spans[i].ptr), spans[i].size));
    }
    if (reduction) {
        // Atomic read-modify-write of the first word, no timestamp
        const u32 payload = regs.report_semaphore_payload;
        const u32 reduction_op = static_cast<u32>(extractBits(data, 9, 3));
        const bool is_signed = extractBits(data, 17, 2) == 1;
        std::atomic_ref<u32> word(*reinterpret_cast<u32*>(spans[0].ptr));
        u32 old = word.load(std::memory_order_relaxed);
        while (true) {
            u32 next;
            switch (reduction_op) {
            case 0:
                next = old + payload;
                break;
            case 1:
                next = is_signed ? static_cast<u32>(std::min(
                                       static_cast<i32>(old),
                                       static_cast<i32>(payload)))
                                 : std::min(old, payload);
                break;
            case 2:
                next = is_signed ? static_cast<u32>(std::max(
                                       static_cast<i32>(old),
                                       static_cast<i32>(payload)))
                                 : std::max(old, payload);
                break;
            case 3:
                next = old >= payload ? 0u : old + 1;
                break;
            case 4:
                next = (old == 0 || old > payload) ? payload : old - 1;
                break;
            case 5:
                next = old & payload;
                break;
            case 6:
                next = old | payload;
                break;
            default:
                next = old ^ payload;
                break;
            }
            if (word.compare_exchange_weak(old, next,
                                           std::memory_order_acq_rel))
                break;
        }
        gpu.getRenderer().invalidateMemory(ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(spans[0].ptr), sizeof(u32)));
        return;
    }
    const auto write = [&](usize begin, const void* data, usize bytes) {
        for (usize i = 0; i < span_count; i++) {
            const auto& span = spans[i];
            const auto first = std::max(begin, span.offset);
            const auto end = std::min(begin + bytes, span.offset + span.size);
            if (first < end)
                std::memcpy(span.ptr + first - span.offset,
                            static_cast<const u8*>(data) + first - begin,
                            end - first);
        }
    };
    if (!short_query) {
        const auto timestamp = getGpuTimestamp();
        write(8, &timestamp, sizeof(timestamp));
    }
    // Publish the result after its timestamp and preceding GPU work.
    std::atomic_thread_fence(std::memory_order_release);
    write(0, &value, short_query ? sizeof(u32) : sizeof(u64));
    for (usize i = 0; i < span_count; i++)
        gpu.getRenderer().invalidateMemory(ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(spans[i].ptr), spans[i].size));
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
    if (format == renderer::TextureFormat::Invalid)
        return nullptr;

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

    const auto format = renderer::toTextureFormat(regs.depth_target_format);
    if (format == renderer::TextureFormat::Invalid)
        return nullptr;

    const auto type = regs.depth_target_array_mode.layers > 1
                          ? renderer::TextureType::_2DArray
                          : renderer::TextureType::_2D;

    const renderer::TextureDescriptor descriptor(
        tls_crnt_gmmu->unmapAddr(gpu_addr), type, format, false, 0,
        regs.depth_target_width, regs.depth_target_height, 1, 1,
        regs.depth_target_array_mode.layers,
        regs.depth_target_tile_mode.width_gobs_log2,
        regs.depth_target_tile_mode.height_gobs_log2,
        regs.depth_target_tile_mode.depth_gobs_log2,
        regs.depth_target_layer_stride * 4);

    return gpu.getRenderer().getTextureCache().find(
        tls_crnt_command_buffer, descriptor, renderer::TextureUsage::Write);
}

void ThreeD::makeRenderTargetKey(std::vector<u8>& key) const {
    key.clear();
    const auto add = [&](const auto& field) {
        const auto* bytes = reinterpret_cast<const u8*>(&field);
        key.insert(key.end(), bytes, bytes + sizeof(field));
    };
    add(regs.color_target_control);
    add(regs.color_targets);
    add(regs.screen_scissor);
    add(regs.depth_target_enabled);
    add(regs.depth_target_addr);
    add(regs.depth_target_format);
    add(regs.depth_target_tile_mode);
    add(regs.depth_target_layer_stride);
    add(regs.depth_target_width);
    add(regs.depth_target_height);
    add(regs.depth_target_array_mode);
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
    const bool lower_left =
        any(regs.window_origin_flags & engines::WindowOriginFlags::LowerLeft);
    const f32 surface_height = static_cast<f32>(regs.screen_scissor.vertical.height);
    bool window_space = false;
    if (viewportTransformEnabled()) {
        auto scale_x = transform.scale_x;
        auto scale_y = transform.scale_y;
        auto offset_y = transform.offset_y;
        // Window rows count from the bottom, memory rows from the top
        if (lower_left) {
            scale_y = -scale_y;
            offset_y = surface_height - offset_y;
        }

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
        res.rect.origin.y() = offset_y - scale_y;
        res.rect.size.x() = scale_x * 2.0f;
        res.rect.size.y() = scale_y * 2.0f;
        // Depth range from the Z scale and offset
        const bool zero_to_one =
            regs.viewport_z_clip == ViewportZClip::ZeroToOne;
        ONCE(LOG_INFO(Engines, "Depth mode: {}",
                      zero_to_one ? "zero to one" : "minus one to one"));
        res.depth_near =
            transform.offset_z - (zero_to_one ? 0.0f : transform.scale_z);
        res.depth_far = transform.offset_z + transform.scale_z;
    } else {
        // The shader outputs window coordinates (see the vertex shader
        // state): the viewport is the surface clip
        window_space = true;
        const auto& screen_scissor = regs.screen_scissor;
        res.rect.origin.x() = screen_scissor.horizontal.x;
        res.rect.origin.y() = screen_scissor.vertical.y;
        res.rect.size.x() = screen_scissor.horizontal.width;
        res.rect.size.y() = screen_scissor.vertical.height;
        res.depth_near = extent.near;
        res.depth_far = extent.far;
    }

    // Flip Y (window space positions are already converted to Metal's
    // orientation)
    if (!window_space) {
        res.rect.origin.y() += res.rect.size.y();
        res.rect.size.y() = -res.rect.size.y();
    }

    // Metal takes depth ranges within 0..1
    res.depth_near = std::clamp(res.depth_near, 0.0f, 1.0f);
    res.depth_far = std::clamp(res.depth_far, 0.0f, 1.0f);

    return res;
}

renderer::Scissor ThreeD::getClearRect() const {
    // Register offsets from NVIDIA's clb197.h
    constexpr u32 REG_CLEAR_RECT_HORIZONTAL = 0x35b;
    constexpr u32 REG_CLEAR_RECT_VERTICAL = 0x35c;
    constexpr u32 REG_CLEAR_CONTROL = 0x43e;
    const u32 control = regs_raw[REG_CLEAR_CONTROL];
    u32 min_x = 0, min_y = 0, max_x = 0xffff, max_y = 0xffff;
    const auto intersect = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
        min_x = std::max(min_x, x0);
        min_y = std::max(min_y, y0);
        max_x = std::min(max_x, x1);
        max_y = std::min(max_y, y1);
    };
    if ((control & ZTD_BIT(4)) != 0) { // Clear rect
        const u32 horizontal = regs_raw[REG_CLEAR_RECT_HORIZONTAL];
        const u32 vertical = regs_raw[REG_CLEAR_RECT_VERTICAL];
        intersect(horizontal & 0xffff, vertical & 0xffff, horizontal >> 16,
                  vertical >> 16);
    }
    if ((control & ZTD_BIT(8)) != 0 && regs.scissors[0].enabled != 0u) {
        const auto& scissor = regs.scissors[0];
        intersect(scissor.horizontal.min, scissor.vertical.min,
                  scissor.horizontal.max, scissor.vertical.max);
    }
    if ((control & ZTD_BIT(12)) != 0) { // Viewport 0 clip
        const auto& viewport = regs.viewports[0];
        intersect(static_cast<u16>(viewport.horizontal.x),
                  static_cast<u16>(viewport.vertical.y),
                  static_cast<u16>(viewport.horizontal.x) +
                      static_cast<u16>(viewport.horizontal.width),
                  static_cast<u16>(viewport.vertical.y) +
                      static_cast<u16>(viewport.vertical.height));
    }
    return renderer::Scissor(uint2({min_x, min_y}),
                             uint2({max_x > min_x ? max_x - min_x : 0,
                                    max_y > min_y ? max_y - min_y : 0}));
}

renderer::Scissor ThreeD::getScissor(u32 index) {
    const auto& scissor = regs.scissors[index];
    if (scissor.enabled != 0u) {
        u32 min_y = scissor.vertical.min;
        u32 max_y = scissor.vertical.max;
        // Window rows count from the bottom, memory rows from the top
        if (any(regs.window_origin_flags &
                engines::WindowOriginFlags::LowerLeft)) {
            const u32 height = regs.screen_scissor.vertical.height;
            const u32 flipped_min = height > max_y ? height - max_y : 0;
            const u32 flipped_max = height > min_y ? height - min_y : 0;
            min_y = flipped_min;
            max_y = flipped_max;
        }
        return renderer::Scissor(
            uint2({scissor.horizontal.min, min_y}),
            uint2({static_cast<u32>(scissor.horizontal.max -
                                    scissor.horizontal.min),
                   max_y > min_y ? max_y - min_y : 0u}));
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
                                        u32 fragment_input_mask,
                                        u32 layer_output_addr) {
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
    // Register offsets from NVIDIA's clb197.h
    constexpr u32 REG_USER_CLIP_ENABLE = 0x544;
    constexpr u32 REG_POINT_SIZE = 0x546;
    constexpr u32 REG_POINT_SPRITE_SELECT = 0x581;
    constexpr u32 REG_ATTRIBUTE_POINT_SIZE = 0x644;
    descriptor.state.program_offset = regs.shader_programs[static_cast<u32>(stage)].offset;
    descriptor.state.y_negate =
        any(regs.window_origin_flags & WindowOriginFlags::LowerLeft);
    // Without the viewport transform the vertex shader outputs window
    // coordinates, converted to clip space in the shader
    if (stage == ShaderStage::VertexB && !viewportTransformEnabled()) {
        descriptor.state.window_space_width = regs.screen_scissor.horizontal.width;
        descriptor.state.window_space_height = regs.screen_scissor.vertical.height;
    }
    if (stage == ShaderStage::VertexB) {
        descriptor.state.fragment_input_mask = fragment_input_mask;
        descriptor.state.layer_output_addr = layer_output_addr;
        descriptor.state.depth_zero_to_one =
            regs.viewport_z_clip == ViewportZClip::ZeroToOne;
        descriptor.state.clip_distance_mask =
            regs_raw[REG_USER_CLIP_ENABLE] & 0xffu;
        descriptor.state.point_list =
            regs.begin.primitive_type == PrimitiveType::Points;
        if (descriptor.state.point_list) {
            descriptor.state.program_point_size =
                (regs_raw[REG_ATTRIBUTE_POINT_SIZE] & 0x1u) != 0;
            descriptor.state.point_size =
                std::bit_cast<f32>(regs_raw[REG_POINT_SIZE]);
        }
    }
    if (stage == ShaderStage::Fragment) {
        descriptor.state.point_coord_origin_bottom =
            ((regs_raw[REG_POINT_SPRITE_SELECT] >> 2) & 0x1u) == 0;
        // SET_ALPHA_TEST, SET_ALPHA_REF, SET_ALPHA_FUNC
        constexpr u32 REG_ALPHA_TEST = 0x4bb;
        constexpr u32 REG_ALPHA_REF = 0x4c4;
        constexpr u32 REG_ALPHA_FUNC = 0x4c5;
        if ((regs_raw[REG_ALPHA_TEST] & 0x1u) != 0) {
            descriptor.state.alpha_test_func =
                decodeCompareOp(regs_raw[REG_ALPHA_FUNC]);
            if (descriptor.state.alpha_test_func != CompareOp::Always &&
                descriptor.state.alpha_test_func != CompareOp::Never)
                descriptor.state.alpha_test_ref =
                    std::bit_cast<f32>(regs_raw[REG_ALPHA_REF]);
        }
    }
    if (stage == ShaderStage::Fragment) {
        descriptor.state.debug_highlight = isHighlightedDraw();
        auto& visual = VisualDiagnostics::instance();
        if (visual.selected()) {
            descriptor.state.visual_output = static_cast<uint32_t>(visual.options().output);
            descriptor.state.visual_input = visual.options().input;
        }
    }

    // Vertex attribute states
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        descriptor.state.vertex_attrib_states[i] = regs.vertex_attrib_states[i];
        descriptor.state.vertex_attrib_states[i].size =
            getFetchSize(regs.vertex_attrib_states[i].size);
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

    // Dual-source blending: only render target 0 is bound, the shader exports
    // the second blend input as output 1 (like Eden)
    if (stage == ShaderStage::Fragment && regs.color_target_control.count != 0 &&
        regs.color_blend_enabled[0] != 0u) {
        const auto is_src1 = [](u32 factor) {
            const auto mapped = getBlendFactor(factor);
            return mapped == renderer::BlendFactor::Src1Color ||
                   mapped == renderer::BlendFactor::InvSrc1Color ||
                   mapped == renderer::BlendFactor::Src1Alpha ||
                   mapped == renderer::BlendFactor::InvSrc1Alpha;
        };
        const auto uses_src1 = [&](const auto& blend) {
            return is_src1(blend.src_rgb_factor) ||
                   is_src1(blend.dst_rgb_factor) ||
                   is_src1(blend.src_alpha_factor) ||
                   is_src1(blend.dst_alpha_factor);
        };
        if (regs.independent_blend_enabled != 0u
                ? uses_src1(regs.independent_blend_state[0])
                : uses_src1(regs.blend_state)) {
            descriptor.state.dual_source_blending = true;
            descriptor.state.color_target_data_types[1] =
                descriptor.state.color_target_data_types[0];
        }
    }

    auto& active_shader =
        active_shaders[static_cast<u32>(toRendererShaderType(stage))];
    descriptor.read_const_word = [&](u32 slot, u32 word) -> std::optional<u32> {
        if (slot >= CONST_BUFFER_BINDING_COUNT)
            return std::nullopt;
        const auto& range =
            bound_const_buffers[static_cast<u32>(stage) - 1][slot];
        if (range.getBegin() == 0 || word >= range.getSize() / sizeof(u32))
            return std::nullopt;
        u32 value;
        std::memcpy(&value,
                    reinterpret_cast<const void*>(range.getBegin() +
                                                  word * sizeof(u32)),
                    sizeof(value));
        return value;
    };
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
            const auto image = getImageHandle(*handle);
            if (image > regs.tex_header_pool_max_index)
                continue;
            const auto sampler = getSamplerHandle(*handle);
            const auto& tic = pool[image];
            if (tic.hdr_version != TicHdrVersion::_1DBuffer &&
                !tic.normalized_coords)
                descriptor.state.unnormalized_textures.push_back(index);
            const auto format =
                renderer::toSampledTextureFormat(tic.format_word, tic.is_srgb);
            if (tic.hdr_version != TicHdrVersion::_1DBuffer &&
                static_cast<u64>(regs.tex_sampler_pool) != 0 &&
                sampler <= regs.tex_sampler_pool_max_index) {
                const auto bytes = tls_crnt_gmmu->readMappedBytes(
                    static_cast<u64>(regs.tex_sampler_pool) + u64(sampler) * sizeof(TextureSamplerControl), sizeof(TextureSamplerControl));
                if (bytes.size() == sizeof(TextureSamplerControl)) {
                    TextureSamplerControl tsc{};
                    std::memcpy(&tsc, bytes.data(), sizeof(tsc));
                    specializeShaderSampler(descriptor.state, index, tsc,
                                            format);
                }
            }
            // Only 1D accesses can be buffer textures. Games can leave stale
            // buffer descriptors in slots the shader samples as another type.
            const auto type_it = resource_mapping.texture_types.find(index);
            const bool is_1d =
                type_it != resource_mapping.texture_types.end() &&
                type_it->second == renderer::TextureType::_1D;
            if (is_1d && tic.hdr_version == TicHdrVersion::_1DBuffer)
                descriptor.state.buffer_texture_formats.emplace(index, format);
            else if (tic.hdr_version != TicHdrVersion::_1DBuffer &&
                     renderer::getIntegerTextureComponentType(format))
                descriptor.state.integer_texture_formats.emplace(index,
                                                                 format);
        }
        if (!descriptor.state.texture_lod_bias.empty() ||
            !descriptor.state.border_samplers.empty() ||
            !descriptor.state.unnormalized_textures.empty() ||
            !descriptor.state.buffer_texture_formats.empty() ||
            !descriptor.state.integer_texture_formats.empty())
            active_shader = gpu.getRenderer().getShaderCache().find(descriptor);
    }

    return active_shader;
}

u32 ThreeD::getGeometryLayerSource() {
    // TODO: support geometry shaders. Until then, handle the common case of a
    // geometry shader that only forwards a vertex output as the layer by
    // writing the layer from the vertex shader
    const auto& program =
        regs.shader_programs[static_cast<usize>(ShaderStage::Geometry)];
    if (!program.config.enable)
        return 0;

    const auto gpu_addr =
        static_cast<u64>(regs.shader_program_region) + program.offset;
    static std::map<u64, u32> layer_sources;
    if (const auto it = layer_sources.find(gpu_addr);
        it != layer_sources.end())
        return it->second;

    const auto code = tls_crnt_gmmu->readMappedBytes(gpu_addr, 0x10000);
    const auto source =
        renderer::shader_decomp::findGeometryLayerSource(code);
    if (source)
        LOG_INFO(Engines,
                 "Geometry shader {:#x} forwards attribute {:#x} as the layer",
                 gpu_addr, *source);
    else
        LOG_WARN(Engines, "Unsupported geometry shader {:#x}", gpu_addr);
    layer_sources[gpu_addr] = source.value_or(0);
    return source.value_or(0);
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
                      : 0,
                  getGeometryLayerSource());

    if (const auto* vertex_shader =
            descriptor.shaders[static_cast<u32>(renderer::ShaderType::Vertex)];
        vertex_shader != nullptr && vertex_shader->getDescriptor().writes_layer) {
        switch (regs.begin.primitive_type) {
        case PrimitiveType::Points:
            descriptor.topology_class = renderer::PrimitiveTopologyClass::Point;
            break;
        case PrimitiveType::Lines:
        case PrimitiveType::LineLoop:
        case PrimitiveType::LineStrip:
        case PrimitiveType::LinesAdjacency:
        case PrimitiveType::LineStripAdjacency:
            descriptor.topology_class = renderer::PrimitiveTopologyClass::Line;
            break;
        default:
            descriptor.topology_class =
                renderer::PrimitiveTopologyClass::Triangle;
            break;
        }
    }

    // Vertex state

    // Vertex attribute states
    bool vertex_arrays_used[VERTEX_ARRAY_COUNT] = {false};
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        const auto& state = regs.vertex_attrib_states[i];
        descriptor.vertex_state.vertex_attrib_states[i] = state;
        descriptor.vertex_state.vertex_attrib_states[i].size =
            getFetchSize(state.size);

        // Set used vertex array
        if (state.type == engines::VertexAttribType::None)
            continue;

        // HACK: how are attributes disabled?
        if (state.is_fixed)
            continue;

        // The stream index has 5 bits, there are 16 streams
        if (state.buffer_id >= VERTEX_ARRAY_COUNT) {
            ONCE(LOG_WARN(Engines, "Vertex attribute {} reads stream {}", i,
                          static_cast<u32>(state.buffer_id)));
            continue;
        }
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
            .is_per_instance = isVertexArrayInstanced(i),
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
            static_cast<bool>(regs.color_blend_enabled[i]) &&
            renderer::getIntegerTextureComponentType(color_target.format) == nullptr &&
            !(VisualDiagnostics::instance().selected() &&
              VisualDiagnostics::instance().options().no_blend);
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

    auto ptr = tls_crnt_gmmu->unmapAddr(vertex_array.addr);
    auto size =
        static_cast<u64>(regs.vertex_array_limits[vertex_array_index]) + 1 -
        static_cast<u64>(vertex_array.addr);
    // Instances are drawn one at a time. An instanced stream advances every
    // `frequency` instances, starting at the base instance; the host reads
    // its element as a constant attribute.
    if (isVertexArrayInstanced(vertex_array_index)) {
        const u64 element = regs.base_instance +
                            instance_index / vertex_array.divisor;
        const u64 offset = element * vertex_array.config.stride;
        if (offset >= size) {
            ONCE(LOG_WARN(Engines,
                          "Instance element {} is past the vertex array end",
                          element));
            return {};
        }
        ptr += offset;
        size -= offset;
    }
    return gpu.getRenderer().getBufferCache().get(
        tls_crnt_command_buffer, ztd::Range<uptr>::fromSize(ptr, size));
}

bool ThreeD::isVertexArrayInstanced(u32 vertex_array_index) const {
    return regs.is_vertex_array_per_instance[vertex_array_index] &&
           regs.vertex_arrays[vertex_array_index].divisor != 0 &&
           regs.vertex_arrays[vertex_array_index].config.stride != 0;
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

    auto type = toTextureType(tic.texture_type);
    u32 depth = tic.depth_minus_one + 1;
    u32 layer_count = 1;
    if (type != renderer::TextureType::_3D) {
        layer_count = depth;
        depth = 1;
        if (type == renderer::TextureType::Cube ||
            type == renderer::TextureType::CubeArray)
            layer_count *= 6;
    }
    const u32 width = tic.width_minus_one + 1;
    const u32 height = tic.height_minus_one + 1;
    u32 level_count = tic.mip_max_levels + 1u;
    if (!renderer::legalizeTextureShape(type, width, height, depth,
                                        layer_count, level_count)) {
        ONCE(LOG_WARN(Engines,
                      "Unsupported {} texture ({}x{}x{}, {} layers)", type,
                      width, height, depth, layer_count));
        return nullptr;
    }
    const renderer::TextureDescriptor descriptor(
        tls_crnt_gmmu->unmapAddr(gpu_addr), type, format, is_linear,
        linear_stride, width, height, depth, level_count, layer_count,
        tic.sparse_tile_width_gobs_log2, tic.tile_height_gobs_log2,
        tic.tile_depth_gobs_log2);

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

    // Depth stencil textures can be sampled through their stencil plane
    const auto view_format =
        is_image ? format
                 : renderer::toSampledTextureFormat(tic.format_word,
                                                    tic.is_srgb);
    // Images store to a single level, the TIC's first view level (e.g. UE4
    // writes every mip of its HZB through images of the same texture).
    // Sampled views keep every level: limiting them to the TIC's view range
    // broke streamed textures.
    const u32 view_min_level =
        std::min(static_cast<u32>(tic.view_mip_min_level), level_count - 1);
    // DEBUG: which images use a nonzero view level
    if (is_image && tic.view_mip_min_level != 0) {
        static std::set<std::tuple<u64, u32, u32>> logged;
        const u64 addr = makeAddr(tic.addr_lo, tic.addr_hi);
        if (logged.insert({addr, tic.view_mip_min_level, level_count}).second)
            LOG_INFO(Engines,
                     "DEBUG image view level {} of {} (addr {:#x}, {} {}x{}, "
                     "hdr {})",
                     static_cast<u32>(tic.view_mip_min_level), level_count,
                     addr, format, tic.width_minus_one + 1,
                     tic.height_minus_one + 1,
                     static_cast<u32>(tic.hdr_version));
    }
    const auto view_levels =
        is_image ? ztd::Range<u32>::fromSize(view_min_level, 1)
                 : ztd::Range<u32>(0, level_count);
    const renderer::TextureViewDescriptor view_descriptor(
        view_type, view_format, view_levels,
        ztd::Range<u32>(0, view_layer_count),
        is_image ? renderer::SwizzleChannels()
                 : renderer::toSampledSwizzleChannels(tic.format_word,
                                                      tic.is_srgb));

    return gpu.getRenderer().getTextureCache().find(
        tls_crnt_command_buffer, descriptor, view_descriptor, usage);
}

namespace {

f32 srgbToLinear(u32 value) {
    const f32 encoded = static_cast<f32>(value) / 255.0f;
    if (encoded <= 0.04045f)
        return encoded / 12.92f;
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

} // namespace

renderer::SamplerDescriptor
decodeSampler(const TextureSamplerControl& tsc,
              renderer::TextureFormat texture_format) {
    constexpr u32 anisotropy[] = {1, 2, 4, 6, 8, 10, 12, 16};
    const auto address = [&](u32 mode) {
        if (mode == static_cast<u32>(renderer::SamplerAddressMode::Clamp))
            return tsc.min_filter == 1 && tsc.mag_filter == 1
                ? renderer::SamplerAddressMode::ClampToEdge
                : renderer::SamplerAddressMode::ClampToBorder;
        return static_cast<renderer::SamplerAddressMode>(mode);
    };
    renderer::SamplerDescriptor descriptor{
        .min_filter = static_cast<renderer::SamplerFilter>(tsc.min_filter),
        .mag_filter = static_cast<renderer::SamplerFilter>(tsc.mag_filter),
        .mip_filter = static_cast<renderer::SamplerMipFilter>(tsc.mip_filter),
        .address_mode_s = address(tsc.address_u),
        .address_mode_t = address(tsc.address_v),
        .address_mode_r = address(tsc.address_p),
        .depth_compare_op = (tsc.depth_compare ? static_cast<CompareOp>(
                                                     tsc.depth_compare_op + 1)
                                               : CompareOp::Invalid),
        .border_color_u = uint4({tsc.border_color_r, tsc.border_color_g,
                                 tsc.border_color_b, tsc.border_color_a}),
        .min_lod = static_cast<f32>(tsc.min_lod_clamp) / 256.0f,
        .max_lod = static_cast<f32>(tsc.max_lod_clamp) / 256.0f,
        .lod_bias = static_cast<f32>(signExtend<i32, 13>(static_cast<i32>(tsc.mip_lod_bias))) / 256.0f,
        .max_anisotropy = anisotropy[tsc.max_anisotropy],
        .integer_texture =
            renderer::getIntegerTextureComponentType(texture_format) != nullptr,
    };

    // sRGB textures use the separate 8-bit border color, converted to linear
    // (alpha is shared). It is only distinct when sRGB conversion is enabled.
    if (tsc.srgb_conversion && renderer::isSrgbTextureFormat(texture_format)) {
        descriptor.border_color_u = uint4(
            {std::bit_cast<u32>(srgbToLinear(tsc.srgb_border_color_r)),
             std::bit_cast<u32>(srgbToLinear(tsc.srgb_border_color_g)),
             std::bit_cast<u32>(srgbToLinear(tsc.srgb_border_color_b)),
             tsc.border_color_a});
    }

    // Integer textures cannot be filtered (Eden uses a nearest variant too)
    if (descriptor.integer_texture) {
        descriptor.min_filter = renderer::SamplerFilter::Nearest;
        descriptor.mag_filter = renderer::SamplerFilter::Nearest;
        if (descriptor.mip_filter == renderer::SamplerMipFilter::Linear)
            descriptor.mip_filter = renderer::SamplerMipFilter::Nearest;
        descriptor.max_anisotropy = 1;
    }

    // The color only matters to samplers addressing the border; clearing it
    // keeps equivalent samplers and shader variants shared
    if (!renderer::samplerUsesBorder(descriptor))
        descriptor.border_color_u = uint4({0u, 0u, 0u, 0u});
    return descriptor;
}

void specializeShaderSampler(renderer::GuestShaderState& state, u32 index,
                             const TextureSamplerControl& tsc,
                             renderer::TextureFormat texture_format) {
    auto descriptor = decodeSampler(tsc, texture_format);
    if (descriptor.lod_bias != 0.0f)
        state.texture_lod_bias.emplace(index, descriptor.lod_bias);
    if (renderer::samplerNeedsBorderEmulation(descriptor)) {
        // The bias is applied separately (texture_lod_bias)
        descriptor.lod_bias = 0.0f;
        state.border_samplers.emplace(index, descriptor);
    }
}

renderer::SamplerBase*
ThreeD::getSampler(const TextureSamplerControl& tsc,
                   renderer::TextureFormat texture_format) const {
    static_assert(sizeof(TextureSamplerControl) == 32);
    std::array<u64, 4> key;
    std::memcpy(key.data(), &tsc, sizeof(key));
    const u64 mixed = (key[0] ^ (key[1] * 0x9e3779b97f4a7c15ull) ^
                       (key[2] * 0xc2b2ae3d27d4eb4full) ^ key[3]) *
                      0x9e3779b97f4a7c15ull;
    // Integer textures (nearest) and sRGB textures (own border color) are
    // memoized apart
    const bool force_nearest =
        renderer::getIntegerTextureComponentType(texture_format) != nullptr;
    const bool srgb_border =
        tsc.srgb_conversion && renderer::isSrgbTextureFormat(texture_format);
    auto& memo = sampler_memos[(force_nearest ? 1u : 0u) |
                               (srgb_border ? 2u : 0u)][mixed >> 56];
    if (memo.sampler != nullptr && memo.key == key)
        return memo.sampler;

    memo = {key, gpu.getRenderer().getSamplerCache().find(
                     decodeSampler(tsc, texture_format))};
    return memo.sampler;
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

    // Storage buffers
    gpu.getRenderer().unbindStorageBuffers(shader_type);
    for (const auto& storage : resource_mapping.storage_buffers) {
        const auto range = resolveStorageBuffer(
            bound_const_buffers[stage_index][storage.cbuf_slot],
            storage.cbuf_offset);
        if (!range) {
            ONCE(LOG_WARN(Engines,
                          "Storage buffer descriptor c{}[{:#x}] is not valid",
                          storage.cbuf_slot, storage.cbuf_offset));
            continue;
        }
        gpu.getRenderer().bindStorageBuffer(
            gpu.getRenderer().getGuestMemoryBuffer(*range), shader_type,
            storage.host_index);
        // The shader may write it: users of the range read the live memory
        if (storage.written) {
            gpu.getRenderer().getBufferCache().markGpuModified(*range);
            gpu.getRenderer().getBufferCache().invalidateMemory(*range);
        }
    }

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
            const auto shader_texture_type =
                type_it != resource_mapping.texture_types.end()
                    ? std::optional(type_it->second)
                    : std::nullopt;
            std::array<u64, 4> tic_words;
            static_assert(sizeof(TextureImageControl) == sizeof(tic_words));
            std::memcpy(tic_words.data(), &tic, sizeof(tic_words));
            const u32 memo_type =
                shader_texture_type ? static_cast<u32>(*shader_texture_type) + 1 : 0;
            TextureMemo* memo =
                renderer_index < TEXTURE_MEMO_SLOTS && stage_index < texture_memo.size()
                    ? &texture_memo[stage_index][renderer_index]
                    : nullptr;
            renderer::ITextureView* texture;
            if (memo != nullptr && memo->view != nullptr &&
                memo->tic == tic_words && memo->type == memo_type) {
                texture = memo->view;
            } else {
                texture = getTexture(tic, shader_texture_type);
                if (memo != nullptr)
                    *memo = {tic_words, memo_type, texture};
            }

            // Sampler
            const auto& tsc = tex_sampler_pool[sampler_handle];
            const auto sampler = getSampler(
                tsc, texture != nullptr ? texture->getDescriptor().format
                                        : renderer::TextureFormat::Invalid);

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
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        const auto& attrib = regs.vertex_attrib_states[i];
        // Unused attributes are fixed with a zero size
        if (i >= 4 && attrib.is_fixed)
            continue;
        str += fmt::format(
            "    attrib{}: buffer={} fixed={} offset={} size={:#x} type={} "
            "bgra={}\n",
            i, static_cast<u32>(attrib.buffer_id), attrib.is_fixed,
            static_cast<u32>(attrib.offset), static_cast<u32>(attrib.size),
            static_cast<u32>(attrib.type), attrib.bgra);
    }
    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++) {
        const auto& vertex_array = regs.vertex_arrays[i];
        if (!vertex_array.config.enable ||
            static_cast<u64>(vertex_array.addr) == 0x0)
            continue;
        // The address may not be mapped (yet); don't fault while tracing
        uptr base;
        const auto as = tls_crnt_gmmu->findAddr(vertex_array.addr, base);
        if (!as || as->ptr == 0x0)
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
            if (tic.hdr_version != TicHdrVersion::BlockLinearColorKey)
                str += fmt::format("[view mips {}..{} lod clamp {:#x}] ",
                                   static_cast<u32>(tic.view_mip_min_level),
                                   static_cast<u32>(tic.view_mip_max_level),
                                   static_cast<u32>(tic.min_lod_clamp));
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
            const auto tex_as = tls_crnt_gmmu->findAddr(
                makeAddr(tic.addr_lo, tic.addr_hi), tex_base);
            if (format != renderer::TextureFormat::Invalid && tex_as &&
                tex_as->ptr != 0x0) {
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

    // DEBUG: dump the color targets before every Nth draw
    // (HYDRA_TRACE_DUMP_RT=<N>, 1 for every draw)
    static const u32 env_dump_rt_every = [] {
        const char* env = std::getenv("HYDRA_TRACE_DUMP_RT");
        return env != nullptr ? static_cast<u32>(std::max(1, std::atoi(env))) : 0u;
    }();
    const u32 dump_rt_every = tracer.getDumpRtEvery() != 0
                                  ? tracer.getDumpRtEvery()
                                  : env_dump_rt_every;
    // HYDRA_TRACE_DUMP_GBUFFER=<N>: dump all targets of every Nth draw that
    // writes 4 or more color targets (the deferred base pass)
    static const u32 env_dump_gbuffer_every = [] {
        const char* env = std::getenv("HYDRA_TRACE_DUMP_GBUFFER");
        return env != nullptr ? static_cast<u32>(std::max(1, std::atoi(env))) : 0u;
    }();
    const u32 dump_gbuffer_every = tracer.getDumpGbufferEvery() != 0
                                       ? tracer.getDumpGbufferEvery()
                                       : env_dump_gbuffer_every;
    static std::atomic<u32> gbuffer_draws{0};
    const bool dump_gbuffer =
        dump_gbuffer_every != 0 && regs.color_target_control.count >= 4 &&
        gbuffer_draws.fetch_add(1) % dump_gbuffer_every == 0;
    // HYDRA_TRACE_DUMP_POST=<N>: dump the target of every Nth draw that writes
    // exactly one full-size color target (post processing and the final pass)
    static const u32 env_dump_post_every = [] {
        const char* env = std::getenv("HYDRA_TRACE_DUMP_POST");
        return env != nullptr ? static_cast<u32>(std::max(1, std::atoi(env))) : 0u;
    }();
    const u32 dump_post_every = tracer.getDumpPostEvery() != 0
                                    ? tracer.getDumpPostEvery()
                                    : env_dump_post_every;
    static std::atomic<u32> post_draws{0};
    const bool dump_post =
        dump_post_every != 0 && regs.color_target_control.count == 1 &&
        post_draws.fetch_add(1) % dump_post_every == 0;
    if (dump_gbuffer || dump_post ||
        (dump_rt_every != 0 && draw_index_for_dump % dump_rt_every == 0)) {
        for (u32 i = 0; i < regs.color_target_control.count; i++) {
            const auto rt_index = regs.color_target_control.getMap(i);
            const auto& rt = regs.color_targets[rt_index];
            if (static_cast<u64>(rt.addr) == 0x0 || rt.height < 400)
                continue;
            std::scoped_lock texture_cache_lock(
                gpu.getRenderer().getTextureCache().getMutex());
            if (const auto view = getColorTargetTexture(rt_index)) {
                if (!tracer.allowImageDump())
                    break;
                const auto path =
                    tracer.getDir() / fmt::format("f{}_d{:04}_rt{}.png",
                                                  tracer.getFrameIndex(),
                                                  draw_index_for_dump, i);
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
                if (view == nullptr || !tracer.markDumped(view) ||
                    !tracer.allowImageDump())
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
    if (renderer::isConstantTextureIndex(binding_index))
        return renderer::getConstantTextureHandle(binding_index);

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

void ThreeD::selectVisualResources() {
    auto& visual = VisualDiagnostics::instance();
    if (!visual.active() || !visual.options().resource)
        return;
    for (u32 i = 0; i < std::min<u32>(static_cast<u32>(regs.color_target_control.count), COLOR_TARGET_COUNT); ++i)
        visual.selectResource(static_cast<u64>(regs.color_targets[regs.color_target_control.getMap(i)].addr));
    if (regs.depth_target_enabled)
        visual.selectResource(static_cast<u64>(regs.depth_target_addr));
    for (const auto stage : {ShaderStage::VertexB, ShaderStage::Fragment}) {
        const auto* shader = getShaderUnchecked(stage);
        if (!shader) continue;
        for (const auto& [binding, host_binding] : shader->getDescriptor().resource_mapping.textures) {
            (void)host_binding;
            const auto handle = readTextureHandle(static_cast<u32>(stage) - 1, binding);
            if (!handle || getImageHandle(*handle) > regs.tex_header_pool_max_index)
                continue;
            const auto bytes = tls_crnt_gmmu->readMappedBytes(
                static_cast<u64>(regs.tex_header_pool) + u64(getImageHandle(*handle)) * sizeof(TextureImageControl),
                sizeof(TextureImageControl));
            if (bytes.size() != sizeof(TextureImageControl)) continue;
            TextureImageControl tic;
            std::memcpy(&tic, bytes.data(), sizeof(tic));
            visual.selectResource(makeAddr(tic.addr_lo, tic.addr_hi));
        }
    }
}

void ThreeD::captureVisualDraw(bool after) {
    auto& visual = VisualDiagnostics::instance();
    if (!visual.selected()) return;
    std::scoped_lock texture_lock(gpu.getRenderer().getTextureCache().getMutex());
    const auto prefix = fmt::format("s{}-p{}-d{}-{}", visual.submissionId(), visual.frameId(), visual.drawIndex(), after ? "after" : "before");
    if (!after) {
        visual.event("draw_state", {
            {"primitive", std::to_string(static_cast<u32>(regs.begin.primitive_type))},
            {"depth_test", std::to_string(regs.depth_test_enabled)},
            {"depth_write", std::to_string(regs.depth_write_enabled)},
            {"depth_compare", std::to_string(static_cast<u32>(regs.depth_compare_op))},
            {"cull_enabled", std::to_string(regs.cull_face_enabled)},
            {"cull_mode", std::to_string(static_cast<u32>(regs.cull_face_mode))},
            {"front_face", std::to_string(static_cast<u32>(regs.front_face_winding))},
            {"base_instance", std::to_string(regs.base_instance + instance_index)},
            {"no_depth", std::to_string(visual.options().no_depth)},
            {"no_cull", std::to_string(visual.options().no_cull)},
            {"no_blend", std::to_string(visual.options().no_blend)}});
        visual.event("depth_stencil_raster", {
            {"depth_target", std::to_string(static_cast<u64>(regs.depth_target_addr))},
            {"depth_target_enabled", std::to_string(regs.depth_target_enabled)},
            {"depth_format", std::to_string(static_cast<u32>(regs.depth_target_format))},
            {"stencil_enabled", std::to_string(regs_raw[0x4e0])},
            {"stencil_func", std::to_string(regs_raw[0x4e4])},
            {"stencil_ref", std::to_string(regs_raw[0x4e5])},
            {"stencil_mask", std::to_string(regs_raw[0x4e6])},
            {"stencil_write_mask", std::to_string(regs_raw[0x4e7])},
            {"depth_bias_fill", std::to_string(regs_raw[0x372])},
            {"depth_bias_constant_bits", std::to_string(regs_raw[0x56f])},
            {"depth_bias_slope_bits", std::to_string(regs_raw[0x55b])},
            {"depth_bias_clamp_bits", std::to_string(regs_raw[0x61f])}});
        visual.event("index_state", {{"address", std::to_string(static_cast<u64>(regs.index_buffer_addr))},
            {"type", std::to_string(static_cast<u32>(regs.index_type))},
            {"start", std::to_string(regs.vertex_elements_start)},
            {"base_vertex", std::to_string(regs.base_vertex)}});
        // Exact guest state is preserved alongside the interpreted fields.
        visual.artifact(prefix + "-registers.bin", std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(regs_raw), sizeof(regs_raw)));
        const auto viewport = getViewport(0);
        const auto scissor = getScissor(0);
        visual.event("viewport_scissor", {
            {"viewport", fmt::format("{},{} {}x{} z={}..{}", viewport.rect.origin.x(),
                viewport.rect.origin.y(), viewport.rect.size.x(), viewport.rect.size.y(),
                viewport.depth_near, viewport.depth_far)},
            {"scissor", fmt::format("{},{} {}x{}", scissor.origin.x(), scissor.origin.y(),
                scissor.size.x(), scissor.size.y())}});
    }
    for (u32 i = 0; i < std::min<u32>(static_cast<u32>(regs.color_target_control.count), COLOR_TARGET_COUNT); ++i) {
        const auto index = regs.color_target_control.getMap(i);
        const auto& rt = regs.color_targets[index];
        if (!after)
            visual.event("render_target", {{"slot", std::to_string(i)},
                {"guest_target", std::to_string(index)}, {"address", std::to_string(static_cast<u64>(rt.addr))},
                {"format", std::to_string(static_cast<u32>(rt.format))},
                {"width_or_stride", std::to_string(rt.width_or_stride)}, {"height", std::to_string(rt.height)},
                {"write_mask", std::to_string(static_cast<u32>(regs.color_write_masks[i]))},
                {"blend_enabled", std::to_string(regs.color_blend_enabled[i])}});
        if (!after) {
            const auto record_blend = [&](const auto& blend) {
                visual.event("blend_state", {{"slot", std::to_string(i)},
                    {"rgb_op", std::to_string(blend.rgb_op)}, {"alpha_op", std::to_string(blend.alpha_op)},
                    {"src_rgb", std::to_string(blend.src_rgb_factor)}, {"dst_rgb", std::to_string(blend.dst_rgb_factor)},
                    {"src_alpha", std::to_string(blend.src_alpha_factor)}, {"dst_alpha", std::to_string(blend.dst_alpha_factor)},
                    {"constant", fmt::format("{},{},{},{}", regs.blend_constant.x(), regs.blend_constant.y(), regs.blend_constant.z(), regs.blend_constant.w())}});
            };
            if (regs.independent_blend_enabled)
                record_blend(regs.independent_blend_state[i]);
            else record_blend(regs.blend_state);
        }
        if (static_cast<u64>(rt.addr) == 0 || !visual.options().images) continue;
        const auto path = visual.imagePath(fmt::format("{}-rt{}", after ? "after" : "before", i));
        if (path.empty()) continue;
        if (auto* view = getColorTargetTexture(index)) {
            visual.event("image_requested", {{"path", path}, {"timing", after ? "after_draw" : "before_draw"}});
            gpu.getRenderer().debugDumpTexture(tls_crnt_command_buffer, view, path);
        }
    }
    if (after) return;
    for (const auto stage : {ShaderStage::VertexB, ShaderStage::Fragment}) {
        const auto* shader = getShaderUnchecked(stage);
        if (!shader) continue;
        const auto& descriptor = shader->getDescriptor();
        const auto& mapping = descriptor.resource_mapping;
        const auto shader_id = VisualDiagnostics::contentId(descriptor.code);
        visual.event("bound_shader", {{"stage", fmt::format("{}", stage)},
            {"object", fmt::format("{}", fmt::ptr(shader))}, {"source_fnv1a64", shader_id},
            {"input_mask", std::to_string(mapping.stage_input_mask)},
            {"sampler_buffer", std::to_string(mapping.sampler_buffer_index)}});
        for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; ++i) {
            if (mapping.uniform_buffers[i] == invalid<u32>()) continue;
            const auto& range = bound_const_buffers[static_cast<u32>(stage) - 1][i];
            visual.event("uniform_binding", {{"stage", fmt::format("{}", stage)},
                {"guest_slot", std::to_string(i)}, {"host_slot", std::to_string(mapping.uniform_buffers[i])},
                {"host_address", std::to_string(range.getBegin())}, {"size", std::to_string(range.getSize())}});
        }
        for (const auto& [binding, host_binding] : mapping.textures) {
            const auto handle = readTextureHandle(static_cast<u32>(stage) - 1, binding);
            if (!handle || getImageHandle(*handle) > regs.tex_header_pool_max_index) {
                visual.event("texture_binding_unavailable", {{"binding", std::to_string(binding)}});
                continue;
            }
            const auto bytes = tls_crnt_gmmu->readMappedBytes(
                static_cast<u64>(regs.tex_header_pool) + u64(getImageHandle(*handle)) * sizeof(TextureImageControl),
                sizeof(TextureImageControl));
            if (bytes.size() != sizeof(TextureImageControl)) {
                visual.event("texture_descriptor_unmapped", {{"binding", std::to_string(binding)}});
                continue;
            }
            TextureImageControl tic;
            std::memcpy(&tic, bytes.data(), sizeof(tic));
            visual.event("texture_binding", {{"stage", fmt::format("{}", stage)},
                {"binding", std::to_string(binding)}, {"host_slot", std::to_string(host_binding)},
                {"handle", std::to_string(*handle)}, {"sampler_index", std::to_string(getSamplerHandle(*handle))},
                {"address", std::to_string(makeAddr(tic.addr_lo, tic.addr_hi))},
                {"width", std::to_string(tic.width_minus_one + 1)},
                {"height", std::to_string(tic.height_minus_one + 1)},
                {"descriptor_fnv1a64", VisualDiagnostics::contentId(bytes)}});
            const auto name = prefix + fmt::format("-{}-binding{}", stage, binding);
            visual.artifact(name + "-tic.bin", std::span<const uint8_t>(bytes));
            if (getSamplerHandle(*handle) <= regs.tex_sampler_pool_max_index) {
                const auto sampler = tls_crnt_gmmu->readMappedBytes(
                    static_cast<u64>(regs.tex_sampler_pool) + u64(getSamplerHandle(*handle)) * sizeof(TextureSamplerControl),
                    sizeof(TextureSamplerControl));
                visual.artifact(name + "-tsc.bin", std::span<const uint8_t>(sampler));
                if (sampler.size() == sizeof(TextureSamplerControl)) {
                    TextureSamplerControl tsc;
                    std::memcpy(&tsc, sampler.data(), sizeof(tsc));
                    visual.event("sampler_binding", {{"binding", std::to_string(binding)},
                        {"stage", fmt::format("{}", stage)},
                        {"min_filter", std::to_string(static_cast<u32>(tsc.min_filter))},
                        {"mag_filter", std::to_string(static_cast<u32>(tsc.mag_filter))},
                        {"mip_filter", std::to_string(static_cast<u32>(tsc.mip_filter))},
                        {"depth_compare", std::to_string(static_cast<u32>(tsc.depth_compare))},
                        {"compare_op", std::to_string(static_cast<u32>(tsc.depth_compare_op))}});
                } else visual.event("sampler_descriptor_unmapped", {{"binding", std::to_string(binding)}});
            }
            if (visual.options().images && tic.hdr_version != TicHdrVersion::_1DBuffer &&
                makeAddr(tic.addr_lo, tic.addr_hi) != 0) {
                const auto type = mapping.texture_types.find(binding);
                const auto path = visual.imagePath(fmt::format("{}-binding{}", stage, binding));
                if (!path.empty()) {
                    if (auto* view = getTexture(tic, type == mapping.texture_types.end() ? std::nullopt : std::optional(type->second))) {
                        visual.event("image_requested", {{"path", path}, {"timing", "bound_texture"}});
                        const auto& base = view->getBase()->getDescriptor();
                        const auto guest_address = makeAddr(tic.addr_lo, tic.addr_hi);
                        if (base.ptr == tls_crnt_gmmu->unmapAddr(guest_address) &&
                            view->getDescriptor().levels.getBegin() == 0 &&
                            view->getDescriptor().layers.getBegin() == 0 &&
                            base.width == tic.width_minus_one + 1 &&
                            base.height == tic.height_minus_one + 1 &&
                            base.getLevelSize(0) <= 1024 * 1024) {
                            const auto guest = tls_crnt_gmmu->readMappedBytes(
                                guest_address, base.getLevelSize(0));
                            visual.artifact(name + "-guest-level0.bin", std::span<const uint8_t>(guest));
                            visual.event("guest_texture_snapshot", {
                                {"artifact", name + "-guest-level0.bin"}, {"image", path},
                                {"layout", fmt::format("{}", base)},
                                {"bytes", std::to_string(guest.size())},
                                {"content_fnv1a64", VisualDiagnostics::contentId(guest)},
                                {"note", "CPU guest bytes may predate GPU writes; snapshot does not flush or overwrite GPU contents"}});
                        }
                        gpu.getRenderer().debugDumpTexture(tls_crnt_command_buffer, view, path);
                    }
                }
            }
        }
    }
}

bool ThreeD::drawInternal() {
    auto& visual = VisualDiagnostics::instance();
    const auto shader_address = [&](ShaderStage stage) {
        return static_cast<u64>(regs.shader_program_region) +
               regs.shader_programs[static_cast<usize>(stage)].offset;
    };
    visual.beginDraw(shader_address(ShaderStage::VertexB), shader_address(ShaderStage::Fragment));
    PerfStats::instance().draws++;

    // State
    if (!regs.shader_programs[static_cast<u32>(ShaderStage::VertexB)]
             .config.enable) {
        LOG_WARN(Engines, "Vertex B stage not enabled, skipping draw");
        return false;
    }

    // Nothing is rasterized: with rasterization off (stream output only), or
    // polygons with both faces culled
    constexpr u32 REG_RASTER_ENABLE = 0xdf;
    if ((regs_raw[REG_RASTER_ENABLE] & 0x1u) == 0 || !shouldExecute())
        return false;
    if (regs.cull_face_enabled != 0 &&
        regs.cull_face_mode == CullFaceMode::FrontAndBack &&
        getPrimitiveClass(regs.begin.primitive_type) == 2)
        return false;

    std::scoped_lock texture_cache_lock(
        gpu.getRenderer().getTextureCache().getMutex());

    // Flush tracked pages
    tls_crnt_gmmu->getMmu()->flushTrackedPages();
    // SET_ZPASS_PIXEL_COUNT (NVB197, byte offset 0x1514).
    gpu.getRenderer().setOcclusionCounterEnabled((regs_raw[0x545] & 1) != 0);

    auto& texture_cache = gpu.getRenderer().getTextureCache();
    const u64 epoch_before_targets = texture_cache.getEpoch();
    // Nothing changed in the texture cache since the previous draw: its
    // render target lookups (which only mark the same targets as written
    // again) and its texture lookups can be reused
    const bool unchanged = epoch_before_targets == texture_memo_epoch &&
                           tls_crnt_gmmu == texture_memo_gmmu &&
                           texture_memo_render_pass != nullptr;
    makeRenderTargetKey(render_target_key);
    renderer::RenderPassBase* render_pass;
    if (unchanged && render_target_key == render_target_memo_key)
        render_pass = static_cast<renderer::RenderPassBase*>(
            const_cast<void*>(texture_memo_render_pass));
    else
        render_pass = getRenderPass();
    texture_memo_valid = epoch_before_targets == texture_memo_epoch &&
                         render_pass == texture_memo_render_pass &&
                         tls_crnt_gmmu == texture_memo_gmmu;
    if (!texture_memo_valid)
        for (auto& stage_memo : texture_memo)
            for (auto& memo : stage_memo)
                memo.view = nullptr;
    auto* pipeline = getPipeline();
    gpu.getRenderer().bindRenderPass(render_pass);
    gpu.getRenderer().bindPipeline(pipeline);
    selectVisualResources();
    if (visual.active())
        visual.event("draw_pipeline", {{"render_pass", fmt::format("{}", fmt::ptr(render_pass))},
            {"pipeline", fmt::format("{}", fmt::ptr(pipeline))},
            {"hot_revision", std::to_string(::hydra::hot::generation())}});

    static const bool highlight_keep_depth =
        std::getenv("HYDRA_HIGHLIGHT_KEEP_DEPTH") != nullptr;
    gpu.getRenderer().setDepthTestEnabled(
        regs.depth_test_enabled != 0u &&
        (highlight_keep_depth || !isHighlightedDraw()) &&
        !(visual.selected() && visual.options().no_depth));
    gpu.getRenderer().setDepthWriteEnabled(regs.depth_write_enabled != 0u &&
        !(visual.selected() && visual.options().no_depth));
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
        const auto decode_func = decodeCompareOp;

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

    // Culling. The guest winding is used as is, FLIP_Y included: inverting it
    // for FLIP_Y culled the front faces of every single-sided mesh in UE4
    // games (Crash Bandicoot 4 floor and characters), while FLIP_Y=0 games
    // (CTR) already cull correctly.
    auto front_face = regs.front_face_winding;
    gpu.getRenderer().setCullState(regs.cull_face_enabled != 0 &&
        !(visual.selected() && visual.options().no_cull),
                                   regs.cull_face_mode, front_face);

    // Depth bias (polygon offset), register offsets from NVIDIA's clb197.h.
    // Each primitive class has its own enable (POINT, LINE, FILL), and the
    // hardware units are twice the host's (Eden and GL halve them too).
    constexpr u32 REG_POLY_OFFSET_POINT = 0x370;
    constexpr u32 REG_SLOPE_SCALE_DEPTH_BIAS = 0x55b;
    constexpr u32 REG_DEPTH_BIAS = 0x56f;
    constexpr u32 REG_DEPTH_BIAS_CLAMP = 0x61f;
    if ((regs_raw[REG_POLY_OFFSET_POINT +
                  getPrimitiveClass(regs.begin.primitive_type)] &
         0x1u) != 0) {
        gpu.getRenderer().setDepthBias(
            std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS]) / 2.0f,
            std::bit_cast<f32>(regs_raw[REG_SLOPE_SCALE_DEPTH_BIAS]),
            std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS_CLAMP]));
    } else {
        gpu.getRenderer().setDepthBias(0.0f, 0.0f, 0.0f);
    }

    // Depth clamps unless the geometry clip mode clips Z (like Eden):
    // PASSTHRU, FRUSTUM_XYZ_CLIP and FRUSTUM_Z_CLIP
    constexpr u32 REG_VIEWPORT_CLIP_CONTROL = 0x64f;
    const u32 geometry_clip =
        extractBits(regs_raw[REG_VIEWPORT_CLIP_CONTROL], 11, 3);
    gpu.getRenderer().setDepthClamp(geometry_clip != 1 && geometry_clip != 3 &&
                                    geometry_clip != 5);

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

        gpu.getRenderer().bindVertexBuffer(getVertexBuffer(i), i);
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
    texture_memo_epoch = texture_cache.getEpoch();
    texture_memo_render_pass = render_pass;
    texture_memo_gmmu = tls_crnt_gmmu;
    render_target_memo_key.swap(render_target_key);

    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::engines

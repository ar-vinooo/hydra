#pragma once

#include "core/hw/tegra_x1/gpu/engines/inline_base.hpp"
#include "core/hw/tegra_x1/gpu/macro/driver_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/draw.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {
class ITextureView;
class SamplerBase;
class RenderPassBase;
class PipelineBase;
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::engines {

enum class TicHdrVersion : u32 {
    _1DBuffer = 0,
    PitchColorKey = 1,
    Pitch = 2,
    BlockLinear = 3,
    BlockLinearColorKey = 4,
};

struct TextureImageControl {
    // 0x00
    ImageFormatWord format_word;

    // 0x04
    u32 addr_lo;

    // 0x08
    u32 addr_hi : 16;
    u32 view_layer_base_3_7 : 5;
    TicHdrVersion hdr_version : 3;
    u32 load_store_hint_maybe : 1;
    u32 view_coherency_hash : 4;
    u32 view_layer_base_8_10 : 3;

    // 0x0C
    union {
        u16 width_minus_one_16_31;
        u16 pitch_5_20;
        struct {
            u16 tile_width_gobs_log2 : 3; // Ignored
            u16 tile_height_gobs_log2 : 3;
            u16 tile_depth_gobs_log2 : 3;
            u16 : 1;
            u16 sparse_tile_width_gobs_log2 : 3;
            u16 gob_3d : 1;
            u16 : 2;
        };
    };
    u16 lod_aniso_quality_2 : 1;
    u16 lod_aniso_quality : 1;            // LodQuality
    u16 lod_iso_quality : 1;              // LodQuality
    u16 aniso_coarse_spread_modifier : 2; // AnisoSpreadModifier
    u16 aniso_spread_scale : 5;
    u16 use_header_opt_control : 1;
    u16 depth_texture : 1;
    u16 mip_max_levels : 4;

    // 0x10
    u32 width_minus_one : 16;
    u32 view_layer_base_0_2 : 3;
    u32 aniso_spread_max_log2 : 3;
    u32 is_srgb : 1;
    TextureType texture_type : 4;
    u32 sector_promotion : 2; // SectorPromotion
    u32 border_size : 3;      // BorderSize

    // 0x14
    u32 height_minus_one : 16;
    u32 depth_minus_one : 14;
    u32 is_sparse : 1;
    u32 normalized_coords : 1;

    // 0x18
    u32 color_key_op : 1;
    u32 trilin_opt : 5;
    u32 mip_lod_bias : 13;
    u32 aniso_bias : 4;
    u32 aniso_fine_spread_func : 2;     // AnisoSpreadFunc
    u32 aniso_coarse_spread_func : 2;   // AnisoSpreadFunc
    u32 max_anisotropy : 3;             // MaxAnisotropy
    u32 aniso_fine_spread_modifier : 2; // AnisoSpreadModifier

    // 0x1C
    union {
        u32 color_key_value;
        struct {
            u32 view_mip_min_level : 4;
            u32 view_mip_max_level : 4;
            u32 msaa_mode : 4; // MsaaMode
            u32 min_lod_clamp : 12;
            u32 : 8;
        };
    };
};

struct TextureSamplerControl {
    // 0x00
    u32 address_u : 3;
    u32 address_v : 3;
    u32 address_p : 3;
    bool32 depth_compare : 1;
    u32 depth_compare_op : 3;
    u32 srgb_conversion : 1;
    u32 font_filter_width : 3;
    u32 font_filter_height : 3;
    u32 max_anisotropy : 3;
    u32 : 9;

    // 0x04
    u32 mag_filter : 3;
    u32 min_lod_clamp_integer_only : 1;
    u32 min_filter : 2;
    u32 mip_filter : 2;
    u32 cubemap_anisotropy : 1;
    u32 cubemap_interface_filtering : 1;
    u32 reduction_filter : 2;
    u32 mip_lod_bias : 13;
    u32 float_coord_normalization : 1;
    u32 trilin_opt : 5;
    u32 : 1;

    // 0x08
    u32 min_lod_clamp : 12;
    u32 max_lod_clamp : 12;
    u32 srgb_border_color_r : 8;

    // 0x0C
    u32 : 12;
    u32 srgb_border_color_g : 8;
    u32 srgb_border_color_b : 8;
    u32 : 4;

    // 0x10
    u32 border_color_r;

    // 0x14
    u32 border_color_g;

    // 0x18
    u32 border_color_b;

    // 0x1C
    u32 border_color_a;
};

// Levels of detail the resource view of a TIC allows. They are absolute mip
// levels of the texture (the TIC address is always level 0), so they clamp the
// sampler's LOD range instead of shifting it
struct TicLodRange {
    u32 min = 0;
    u32 max = 15;
};

TicLodRange getTicLodRange(const TextureImageControl& tic);

// Memory layout a TIC describes. The color key versions share the layout of
// their plain versions (clb197tex.h: the key sits in the last words); other
// header versions have no known layout.
struct TicLayout {
    bool is_linear;
    u32 linear_stride;
};
std::optional<TicLayout> getTicLayout(const TextureImageControl& tic);
MTL::StencilOperation decodeStencilOp(u32 value);

// The texture format selects the border color interpretation (integer bits,
// sRGB border); Invalid for none
renderer::SamplerDescriptor
decodeSampler(const TextureSamplerControl& tsc,
              renderer::TextureFormat texture_format =
                  renderer::TextureFormat::Invalid,
              TicLodRange view_lods = {});

// Records the guest sampler state the shader must be specialized for (LOD
// bias, border emulation) in the shader state of a texture binding
void specializeShaderSampler(renderer::GuestShaderState& state, u32 index,
                             const TextureSamplerControl& tsc,
                             renderer::TextureFormat texture_format,
                             TicLodRange view_lods);

struct RenderTarget {
    Iova addr;
    u32 width_or_stride;
    u32 height;
    ColorSurfaceFormat format;
    struct {
        u32 width_gobs_log2 : 4;
        u32 height_gobs_log2 : 4;
        u32 depth_gobs_log2 : 4;
        bool32 is_linear : 1;
        u32 unused : 3;
        bool32 is_3d : 1;
    } tile_mode;
    struct {
        u16 layers;
        bool volume : 1; // TODO: what is this?
    } array_mode;
    u32 layer_stride;
    u32 base_layer;
    u32 mark; // TODO: what is this?
    u32 padding[6];
};

// Depth mode (clip space Z range)
enum class ViewportZClip : u32 {
    MinusOneToOne,
    ZeroToOne,
};

enum class WindowOriginFlags : u32 {
    None = 0,
    LowerLeft = ZTD_BIT(0),
    FlipY = ZTD_BIT(4), // Only for the purpose of figuring out polygon winding
};
ZTD_ENABLE_ENUM_BITWISE_OPERATORS(WindowOriginFlags)

enum class ViewportSwizzle : u32 {
    PositiveX = 0,
    NegativeX = 1,
    PositiveY = 2,
    NegativeY = 3,
    PositiveZ = 4,
    NegativeZ = 5,
    PositiveW = 6,
    NegativeW = 7,
};

// TODO: handle this differently
inline renderer::ShaderType toRendererShaderType(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::VertexB:
        return renderer::ShaderType::Vertex;
    case ShaderStage::Fragment:
        return renderer::ShaderType::Fragment;
    case ShaderStage::Compute:
        return renderer::ShaderType::Compute;
    case ShaderStage::Geometry:
        return renderer::ShaderType::Geometry;
    case ShaderStage::TessCtrl:
        return renderer::ShaderType::TessControl;
    case ShaderStage::TessEval:
        return renderer::ShaderType::TessEvaluation;
    default:
        ONCE(LOG_NOT_IMPLEMENTED(Engines, "Shader stage {}", stage));
        return renderer::ShaderType::Count;
    }
}

struct Regs3D {
    RegsInline regs_inline;

    u32 padding_0x6e[0x192];

    // 0x200 Render targets
    RenderTarget color_targets[COLOR_TARGET_COUNT];

    // 0x280 Viewport transforms
    struct {
        f32 scale_x;
        f32 scale_y;
        f32 scale_z;
        f32 offset_x;
        f32 offset_y;
        f32 offset_z;
        struct {
            ViewportSwizzle x : 3;
            u32 unused_x0_x3 : 1;
            ViewportSwizzle y : 3;
            u32 unused_x0_x7 : 1;
            ViewportSwizzle z : 3;
            u32 unused_x0_x11 : 1;
            ViewportSwizzle w : 3;
            u32 unused_x0_x15 : 1;
            u32 unused_x0_x16 : 16;
        } swizzle;
        struct {
            u32 x : 5;
            u32 unused1 : 3;
            u32 y : 5;
            u32 unused2 : 19;
        } subpixel_precision_bias;
    } viewport_transforms[VIEWPORT_COUNT];

    // 0x300 Viewports
    struct {
        struct {
            i16 x;
            i16 width;
        } horizontal;
        struct {
            i16 y;
            i16 height;
        } vertical;
        f32 near;
        f32 far;
    } viewports[VIEWPORT_COUNT];

    // 0x340 Window rectangle
    struct {
        struct {
            u16 min;
            u16 max;
        } horizontal;
        struct {
            u16 min;
            u16 max;
        } vertical;
    } window_rects[8];

    u32 padding2[0xd];

    // 0x35d
    u32 vertex_array_start;

    u32 not_a_register_0x35e;

    // 0x35f
    ViewportZClip viewport_z_clip;

    // 0x360 Clear data
    uint4 clear_color;
    f32 clear_depth;

    u32 padding_0x365[0x2];

    // 0x367
    bool32 color_reduction_enable;
    u32 clear_stencil;

    u32 padding_0x369[0xb];

    // 0x374
    bool32 advanced_blend_enabled;

    u32 padding_0x375[0xb];

    // 0x380 scissor
    struct {
        bool32 enabled;
        struct {
            u16 min;
            u16 max;
        } horizontal;
        struct {
            u16 min;
            u16 max;
        } vertical;
        u32 _padding;
    } scissors[VIEWPORT_COUNT];

    u32 padding_0x3c0[0x30];
    u32 padding_0x3f0[0x8];

    // 0x3f8 depth target
    Iova depth_target_addr;
    DepthSurfaceFormat depth_target_format;
    struct {
        u32 width_gobs_log2 : 4;
        u32 height_gobs_log2 : 4;
        u32 depth_gobs_log2 : 4;
        u32 padding : 20;
    } depth_target_tile_mode;
    u32 depth_target_layer_stride;

    // 0x3fd screen scissor
    struct {
        struct {
            u16 x;
            u16 width;
        } horizontal;
        struct {
            u16 y;
            u16 height;
        } vertical;
    } screen_scissor;

    u32 padding_0x3ff[0x51];
    u32 padding_0x450[0x8];

    // 0x458 vertex attribute states
    VertexAttribState vertex_attrib_states[VERTEX_ATTRIB_COUNT];

    u32 padding_0x478[0xf];

    // 0x487 color target control
    struct {
        u32 count : 4;
        u32 maps : 24;

        u32 getMap(u32 index) const { return (maps >> (index * 3)) & 0x7; }
    } color_target_control;

    u32 padding_0x488[0x2];

    // 0x48a depth target dimensions
    u32 depth_target_width;
    u32 depth_target_height;
    struct {
        u16 layers;
        bool volume;
    } depth_target_array_mode;

    // 0x48d SET_SAMPLER_BINDING: bit 0 set means the sampler index is the
    // texture header index (the handle's sampler bits are ignored)
    u32 sampler_binding;

    u32 padding_0x48e[0x25];

    // 0x4b3
    bool32 depth_test_enabled;

    u32 padding_0x4b4[0x5];

    // 0x4b9
    bool32 independent_blend_enabled;
    bool32 depth_write_enabled;

    u32 padding_0x4bb[0x8];

    // 0x4c3
    CompareOp depth_compare_op;

    u32 padding_0x4c4[0x3];

    // 0x4c7
    float4 blend_constant;

    u32 padding_0x4cb[0x4];

    // 0x4cf
    struct {
        // SET_BLEND_SEPARATE_FOR_ALPHA (clb197.h 0x133c): bit 0
        u32 separate_for_alpha;
        u32 rgb_op;
        u32 src_rgb_factor;
        u32 dst_rgb_factor;
        u32 alpha_op;
        u32 src_alpha_factor;
        u32 global_color_key; // TODO: what is this?
        u32 dst_alpha_factor;
    } blend_state;
    u32 single_rop_control;
    u32 color_blend_enabled[COLOR_TARGET_COUNT];

    u32 padding_0x4e0[0xb];

    // 0x4eb
    WindowOriginFlags window_origin_flags;

    u32 padding_0x4ec[0x21];

    // 0x50d
    u32 base_vertex;
    u32 base_instance;

    u32 padding_0x50f[0x3f];

    // 0x54e
    bool32 depth_target_enabled;

    u32 padding_0x54f[0x8];

    // 0x557
    Iova tex_sampler_pool;
    u32 tex_sampler_pool_max_index;

    u32 padding_0x55a[0x3];

    // 0x55d
    Iova tex_header_pool;
    u32 tex_header_pool_max_index;

    u32 padding_0x560[0x22];

    // 0x582
    Iova shader_program_region;
    u32 attribute_default; // TODO: what is this?

    u32 end;
    struct {
        PrimitiveType primitive_type : 16;
        u32 : 8;
        bool32 primitive_id_reset : 1;
        u32 : 1;
        bool32 instance_next : 1;
        bool32 instance_ctrl : 1; // TODO: is the name correct?
    } begin;

    u32 padding_0x587[0x9];

    u32 padding_0x590;
    u32 primitive_restart_enabled;
    u32 primitive_restart_index;
    u32 padding_0x593[0x5f];

    // 0x5f2 indexed draws
    Iova index_buffer_addr;
    Iova index_buffer_limit_addr;

    IndexType index_type;

    u32 vertex_elements_start;
    u32 not_a_register_0x5f8;

    u32 padding_0x5f9[0x27];

    // 0x620
    bool32 is_vertex_array_per_instance[VERTEX_ARRAY_COUNT];

    u32 padding_0x630[0x16];

    // 0x646
    bool32 cull_face_enabled;
    Winding front_face_winding;
    CullFaceMode cull_face_mode;

    u32 padding_0x649[0x2];

    // 0x64b
    bool32 viewport_transform_enabled;

    u32 padding_0x64c[0x34];

    // 0x680
    ColorWriteMask color_write_masks[COLOR_TARGET_COUNT];

    u32 padding_0x688[0x38];

    // 0x6c0 report semaphore
    Iova report_semaphore_addr;
    u32 report_semaphore_payload;
    u32 report_semaphore_todo; // TODO: should this be a reg?

    u32 padding_0x6c4[0x3c];

    // 0x700 vertex arrays
    struct {
        struct {
            u32 stride : 12;
            bool32 enable : 1;
            u32 padding : 19;
        } config;
        Iova addr;
        u32 divisor;
    } vertex_arrays[VERTEX_ARRAY_COUNT];

    u32 padding_0x740[0x40];

    // 0x780
    struct {
        // SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA (clb197.h 0x1e00 + j * 32)
        u32 separate_for_alpha;
        u32 rgb_op;
        u32 src_rgb_factor;
        u32 dst_rgb_factor;
        u32 alpha_op;
        u32 src_alpha_factor;
        u32 dst_alpha_factor;
        u32 padding;
    } independent_blend_state[COLOR_TARGET_COUNT];

    Iova vertex_array_limits[VERTEX_ARRAY_COUNT];

    u32 padding_0x7e0[0x20];

    // 0x800 shader programs
    struct {
        struct {
            bool32 enable : 1;
            u32 padding1 : 3;
            ShaderStage stage : 4;
            u32 padding2 : 24;
        } config;
        u32 offset;
        u32 padding1;
        u32 num_registers;
        u32 padding2[0xc];
    } shader_programs[static_cast<u32>(ShaderStage::Count)];

    u32 padding_0x860[0x80];

    // 0x8e0 constant buffers
    u32 const_buffer_selector_size;
    Iova const_buffer_selector;

    u32 load_const_buffer_offset;
    u32 not_a_register_0x8e4[0x10];

    u32 padding_0x8f4[0xc];
    u32 padding_0x900[0x82];

    // 0x982
    u32 bindless_texture_const_buffer_slot;

    u32 padding_0x983[0x37d];

    // 0xd00
    u32 mme_scratch[0x100];
};
static_assert(sizeof(Regs3D) == MACRO_METHODS_REGION * sizeof(u32));

// Preserve the explicitly programmed color and alpha equations. The NVIDIA
// header names SEPARATE_FOR_ALPHA but does not specify a draw-time override of
// the alpha registers. Inferring that override from glBlendFunc discarded the
// captured ADD-color / REVERSE_SUBTRACT-alpha light-shaft occlusion equation.
// Register-write side effects of SEPARATE_FOR_ALPHA still need hardware proof.
struct BlendEquation {
    u32 rgb_op;
    u32 src_rgb_factor;
    u32 dst_rgb_factor;
    u32 alpha_op;
    u32 src_alpha_factor;
    u32 dst_alpha_factor;
};

MTL::BlendOperation getBlendOperation(u32 blend_op);
MTL::BlendFactor getBlendFactor(u32 blend_factor);
void invalidateShaderInstructions(Gpu& gpu, u32 flags);

template <typename Blend>
inline BlendEquation getBlendEquation(const Blend& blend) {
    return {blend.rgb_op,   blend.src_rgb_factor,   blend.dst_rgb_factor,
            blend.alpha_op, blend.src_alpha_factor, blend.dst_alpha_factor};
}

class ThreeD : public EngineWithRegsBase<Regs3D>,
               public InlineBase,
               public macro::MacroHost {
    // Lets tests read the viewport and scissor conversions
    friend struct ThreeDTestAccess;

  public:
    explicit ThreeD(Gpu& gpu_);

    void method(u32 method, u32 arg) override;
    void methodHot(u32 method, u32 arg);

    void flushMacro() override;
    void flushMacroHot();

    // Getters
    const Regs3D& getRegs() const { return regs; }
    // Resolves a binding against the current GPU address space, gathering
    // separate host mappings into owned CPU memory when necessary.
    ztd::Range<uptr> getConstBuffer(u32 stage_index, u32 slot,
                                   u64 required_size = 0) const;
    renderer::BufferView getConstBufferUpload(u32 stage_index, u32 slot,
                                             u64 required_size = 0);
    // Conditional rendering applies to draws and clears.
    bool shouldExecute() const;
    const std::vector<u32>& getInlineIndices() const { return inline_indices; }

    // Macros
    u32 readMacroRegister(u32 reg) override { return getReg(reg); }
    void callMacroMethod(u32 method, u32 arg) override {
        if (!isConstBufferData(method))
            flushConstBufferInvalidation();
        this->method(method, arg);
    }

    // LOAD_CONST_BUFFER words invalidate the buffer cache once per run of
    // words: any other method may read the buffer, so it flushes them first
    static constexpr bool isConstBufferData(u32 method) {
        return method >= 0x8e4 && method < 0x8e4 + 16;
    }
    void flushConstBufferInvalidation() {
        if (pending_const_buffer_invalidation.getSize() != 0)
            flushConstBufferInvalidationImpl();
    }

    // Helpers (also used by the compute engine)
    // Write usage is for storage images
    renderer::ITextureView*
    getTexture(const TextureImageControl& tic,
               std::optional<MTL::TextureType> shader_type = std::nullopt,
               renderer::TextureUsage usage = renderer::TextureUsage::Read) const;
    renderer::SamplerBase*
    getSampler(const TextureSamplerControl& tsc,
               renderer::TextureFormat texture_format =
                   renderer::TextureFormat::Invalid,
               TicLodRange view_lods = {}) const;
    // Samplers are immutable and never evicted, so they are memoized by the
    // raw TSC words instead of hashing a descriptor for every binding
    struct SamplerMemo {
        std::array<u64, 4> key{};
        u32 view_lods{0};
        renderer::SamplerBase* sampler{nullptr};
    };
    // Indexed by integer texture | sRGB border << 1
    mutable std::array<std::array<SamplerMemo, 256>, 4> sampler_memos{};
    // Texture lookups of the previous draws, reused while the texture cache
    // epoch only changed by this draw's own render target lookups
    struct TextureMemo {
        std::array<u64, 4> tic{};
        u32 type{0};
        renderer::ITextureView* view{nullptr};
        std::vector<resource::MappingDependency> mappings;
    };
    static constexpr u32 TEXTURE_MEMO_SLOTS = 32;
    std::array<std::array<TextureMemo, TEXTURE_MEMO_SLOTS>, 5> texture_memo{};
    u64 texture_memo_epoch{~0ull};
    u64 texture_memo_mapping_generation{0};
    resource::ResourceHandle texture_memo_address_space;
    std::vector<resource::MappingDependency> render_target_memo_mappings;
    std::map<u64, u32> geometry_layer_sources;
    const void* texture_memo_render_pass{nullptr};
    const void* texture_memo_gmmu{nullptr};
    bool texture_memo_valid{false};
    // Registers the render target lookups read: with the same values and an
    // unchanged epoch, the render pass of the previous draw is reused
    std::vector<u8> render_target_key;
    std::vector<u8> render_target_memo_key;
    void makeRenderTargetKey(std::vector<u8>& key) const;

  protected:
    void macro(u32 method, u32 arg) override;

  private:
    Gpu& gpu;

    // Macros
    std::unique_ptr<macro::DriverBase> macro_driver;

    // Active state (for quick access)
    renderer::ShaderBase* active_shaders[static_cast<usize>(
        renderer::ShaderType::Count)] = {nullptr};

    // State
    // GPU virtual ranges, resolved when consumed rather than at bind time.
    ztd::Range<uptr>
        bound_const_buffers[static_cast<usize>(ShaderStage::Count) - 1]
                           [CONST_BUFFER_BINDING_COUNT];
    struct GatheredConstBuffer {
        std::vector<u8> bytes;
        renderer::BufferView upload;
    };
    mutable GatheredConstBuffer
        gathered_const_buffers[static_cast<usize>(ShaderStage::Count) - 1]
                              [CONST_BUFFER_BINDING_COUNT];

    // Methods
    DEFINE_INLINE_ENGINE_METHODS;

    void incrementSyncPoint(u32 index, u32 arg);
    void noteUnimplementedMultisample();
    void invalidateShaderCaches(u32 index, u32 flags);

    void loadMmeInstructionRamPointer(const u32 index, const u32 ptr);
    void loadMmeInstructionRam(const u32 index, const u32 data);
    void loadMmeStartAddressRamPointer(const u32 index, const u32 ptr);
    void loadMmeStartAddressRam(const u32 index, const u32 data);

    // Instance of the current draw
    u32 instance_index{0};
    std::vector<u32> inline_indices;
    void beginDraw(const u32 index, const u32 arg);
    void endDraw(const u32 index, const u32 arg);
    void inlineIndex32(const u32 index, const u32 arg);
    void inlineIndex16(const u32 index, const u32 arg);
    void inlineIndex8(const u32 index, const u32 arg);
    void drawVertexElementsImpl(u32 count,
                               std::optional<ztd::Range<uptr>> inline_range);
    void drawAuto(const u32 index, const u32 byte_count);
    void drawZeroIndex(const u32 index, const u32 count);
    // DRAW_ZERO_INDEX's indices
    std::shared_ptr<renderer::BufferBase> zero_indices;
    // SET_CT_SELECT's target count, at most the 8 targets
    u32 getColorTargetCount() const {
        return std::min<u32>(regs.color_target_control.count,
                             COLOR_TARGET_COUNT);
    }
    // A render enable condition (SET_RENDER_ENABLE_A, or the global one)
    bool evaluateRenderEnable(u32 base_reg) const;
    void drawVertexArray(const u32 index, u32 count);
    void drawVertexElements(const u32 index, u32 count);
    // Draws with their topology, first vertex or index and count in the
    // argument. The first draw of an instance group restarts the instances.
    void drawVertexArrayCompact(const u32 index, const u32 arg);
    void drawVertexElementsCompact(const u32 index, const u32 arg);

    struct ClearBufferData {
        bool depth : 1;
        bool stencil : 1;
        u8 color_mask : 4;
        u32 target_id : 4;
        u32 layer_id : 16;
    };

    // Area a clear is restricted to by the clear control register
    MTL::ScissorRect getClearRect() const;

    void clearBuffer(const u32 index, const ClearBufferData data);

    // HACK
    void resetCounter(const u32 index, const u32 data);
    void setReportSemaphore(const u32 index, const u32 data);
    // Reports the ZPASS counter without waiting for the GPU
    void queueCounterReport(u64 address, bool short_query);
    // Checks that the memory of a report exists and settles what the GPU
    // wrote there. Returns the host ranges of the report.
    bool prepareReportWrite(u64 address, usize size,
                            std::vector<ztd::Range<uptr>>& writes);

    void firmwareCall4(const u32 index, const u32 data);

    void loadConstBuffer(const u32 index, const u32 data);
    // Host range written by LOAD_CONST_BUFFER and not invalidated yet
    ztd::Range<uptr> pending_const_buffer_invalidation;
    void flushConstBufferInvalidationImpl();
    void bindGroup(const u32 index, const u32 data);

    // Helpers
    renderer::ITextureView*
    getColorTargetTexture(u32 render_target_index) const;
    renderer::ITextureView* getDepthStencilTargetTexture() const;
    renderer::MsaaLayout getRenderTargetMsaaLayout() const;
    // The TSC index of a texture handle (see SET_SAMPLER_BINDING)
    u32 getSamplerIndex(u32 handle) const;
    ColorWriteMask getColorWriteMask(u32 render_target_index) const;

    // Reads the handle a shader texture binding refers to
    std::optional<u32> readTextureHandle(u32 stage_index,
                                         u32 binding_index) const;
    renderer::RenderPassBase* getRenderPass() const;
    // DEBUG: frame tracer
    void traceDraw(const char* kind, u32 count, u32 first, u32 base_vertex);
    bool isHighlightedDraw() const;
    static bool isProfilingDraws();
    void profileDraw(u32 count);
    void labelCaptureDraw(u32 count);
    u32 capture_draw_index{0};

    MTL::Viewport getViewport(u32 index);
    MTL::ScissorRect getScissor(u32 index);
    // The scissor alone, without the surface clip
    MTL::ScissorRect getScissorRect(u32 index);
    renderer::ShaderBase* getShaderUnchecked(ShaderStage stage) const;
    renderer::ShaderBase* getShader(ShaderStage stage,
                                    u32 fragment_input_mask = 0,
                                    u32 layer_output_addr = 0);
    u32 getGeometryLayerSource();
    // Geometry shader emulation (see renderer/geometry.hpp)
    bool useGeometryShader() const;
    renderer::ShaderBase*
    getGeometryPassthrough(const renderer::ShaderBase& geometry_shader,
                           u32 fragment_input_mask);
    // The current pipeline has an emulated geometry shader
    bool geometry_active{false};
    // Tessellation emulation (see renderer/tessellation.hpp)
    bool useTessellation() const;
    renderer::ShaderBase* getTessellationCopy(u32 patch_size);
    renderer::ShaderBase* getTessellationFactors(u32 domain, bool from_patches,
                                                 const std::array<f32, 6>& levels);
    bool tessellation_active{false};
    // Control points the control shader of the current pipeline outputs
    u32 tessellation_control_points{0};
    std::map<u32, std::unique_ptr<renderer::ShaderBase>> tessellation_copies;
    std::map<std::tuple<u32, bool, std::array<u32, 6>>,
             std::unique_ptr<renderer::ShaderBase>>
        tessellation_factor_shaders;
    std::map<std::tuple<u32, bool, bool, bool, u32, u32, u32, bool, bool, u32>,
             std::unique_ptr<renderer::ShaderBase>>
        geometry_passthroughs;
    renderer::PipelineBase* getPipeline();
    renderer::BufferView getVertexBuffer(u32 vertex_array_index) const;
    bool isVertexArrayInstanced(u32 vertex_array_index) const;

    void configureShaderStage(const ShaderStage stage,
                              const TextureImageControl* tex_header_pool,
                              const TextureSamplerControl* tex_sampler_pool,
                              command_ir::StageBindings& bindings,
                              std::vector<ztd::Range<uptr>>& storage_writes);

    renderer::DrawState prepareDrawState();
    renderer::RenderPassBase* resolveDrawTargets();
    bool prepareDraw(command_ir::DrawCommand& command);
    void executeDrawCommand(command_ir::DrawCommand& command);
    // Whether the draw can be encoded on the draw queue's thread
    bool canQueueDraw(const command_ir::DrawCommand& command, bool capturing);
    void selectVisualResources();
    // One line per texture the bound shaders sample, from the guest TIC/TSC
    std::vector<std::string> describeTextureBindings() const;
    void captureVisualDraw(bool after);
};

} // namespace hydra::hw::tegra_x1::gpu::engines

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::TicHdrVersion,
                       _1DBuffer, "1D buffer", PitchColorKey, "pitch color key",
                       Pitch, "pitch", BlockLinear, "block linear",
                       BlockLinearColorKey, "block linear color key")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::ViewportSwizzle,
                       PositiveX, "positive X", NegativeX, "negative X",
                       PositiveY, "positive Y", NegativeY, "negative Y",
                       PositiveZ, "positive Z", NegativeZ, "negative Z",
                       PositiveW, "positive W", NegativeW, "negative W")

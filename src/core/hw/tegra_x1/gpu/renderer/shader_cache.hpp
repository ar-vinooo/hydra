#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <tuple>

namespace hydra::hw::tegra_x1::gpu::renderer {

class ShaderBase;
class IRenderer;

struct GuestShaderState {
    u32 program_offset{0};
    // VertexA: the program the vertex shader (VertexB) continues, with its
    // header (set by the shader cache, null when the stage is disabled)
    std::shared_ptr<const std::vector<u8>> vertex_a_code;
    u32 vertex_a_program_offset{0};
    // A geometry shader (emulated, see geometry.hpp): the input primitive of
    // the draw (geometry_input_vertices is 0 for other stages)
    u32 geometry_input_vertices{0};
    engines::PrimitiveType geometry_input_primitive{
        engines::PrimitiveType::Points};
    // A vertex shader of a geometry pipeline writes its outputs to a buffer
    bool geometry_capture{false};
    // Tessellation (emulated, see tessellation.hpp): control points per
    // input patch, those the control shader outputs (for the evaluation
    // shader) and the domain
    u32 tessellation_patch_size{0};
    u32 tessellation_control_points{0};
    u32 tessellation_domain{0};
    std::map<std::pair<u32, u32>, u32> branch_constants;
    std::map<u32, TextureFormat> buffer_texture_formats;
    // Sampled (non-buffer) textures with integer formats
    std::map<u32, TextureFormat> integer_texture_formats;
    std::map<u32, f32> texture_lod_bias;
    // Types of textures the shader only queries the size of. No sampling
    // instruction tells their type, so the guest descriptor's type is used
    std::map<u32, MTL::TextureType> texture_type_hints;
    // Textures whose sampler border handling Metal cannot express (custom
    // border colors, integer borders, mirror to border). The shader samples
    // them with its own samplers and blends the border in
    std::map<u32, SamplerDescriptor> border_samplers;
    std::vector<u32> unnormalized_textures;
    // User inputs read by the fragment shader, which the vertex shader must
    // write (bit per location)
    u32 fragment_input_mask{0};
    // DEBUG: output a solid color from the fragment shader
    bool debug_highlight{false};
    uint32_t visual_output{0};
    uint32_t visual_input{0};
    // Clip space Z is already in the [0, 1] range
    bool depth_zero_to_one{false};
    // Output attribute address the vertex shader exports as the render target
    // layer, in place of an unsupported geometry shader that copies it (0 if
    // none)
    u32 layer_output_addr{0};
    // Clip distances enabled by the guest (bit per distance)
    u32 clip_distance_mask{0};
    // Points are drawn: the vertex shader always exports a point size, its
    // own when program point size is enabled, otherwise point_size
    bool point_list{false};
    bool program_point_size{false};
    f32 point_size{1.0f};
    // The window origin is lower left (SR_Y_DIRECTION reads -1.0)
    bool y_negate{false};
    // Size of the window the vertex shader outputs positions in (0 when it
    // outputs clip space)
    u32 window_space_width{0};
    u32 window_space_height{0};
    // Point sprite coordinates have their origin at the bottom
    bool point_coord_origin_bottom{false};
    // Depth and stencil tests run before the fragment shader (and its
    // discards), SET_API_MANDATED_EARLY_Z
    bool early_fragment_tests{false};
    // Fixed function alpha test of render target 0 (Invalid when disabled)
    engines::CompareOp alpha_test_func{engines::CompareOp::Invalid};
    f32 alpha_test_ref{0.0f};
    // Render target 0 blends with the second color output (SRC1 factors): the
    // shader's output 1 is that color
    bool dual_source_blending{false};
    engines::VertexAttribState vertex_attrib_states[VERTEX_ATTRIB_COUNT];
    MTL::DataType color_target_data_types[COLOR_TARGET_COUNT] = {
        MTL::DataTypeNone};
    // Channel rearrangement for alpha-only/ARGB guest formats (ColorOutputSwizzle)
    u8 color_target_swizzles[COLOR_TARGET_COUNT] = {};
    // Compute
    u32 shared_memory_size{0};
    std::map<u32, TextureFormat> image_formats;
};

struct GuestShaderDescriptor {
    engines::ShaderStage stage;
    uptr code_ptr;
    uptr gpu_addr;
    GuestShaderState state;
    std::function<std::optional<u32>(u32, u32)> read_const_word;
    // Filled by ShaderCache::find: compilation and identity use one snapshot.
    std::shared_ptr<const std::vector<u8>> code_snapshot;
    std::vector<u32> cache_key;
    u64 code_revision{0};
    // VertexA program of a vertex shader (0 when the stage is disabled)
    uptr vertex_a_gpu_addr{0};
    u64 vertex_a_code_revision{0};
};

class ShaderCache
    : public CacheBase<ShaderCache, ShaderBase*, GuestShaderDescriptor, true, true> {
  public:
    explicit ShaderCache(IRenderer& renderer_) : renderer{renderer_} {}

    void destroy() {}

    ShaderBase* find(const GuestShaderDescriptor& descriptor);
    ShaderBase* create(const GuestShaderDescriptor& descriptor);
    void update([[maybe_unused]] ShaderBase* shader) {}
    static u32 hash(const GuestShaderDescriptor& descriptor);
    static bool equals(const GuestShaderDescriptor& a,
                       const GuestShaderDescriptor& b);
    // Force a new owned code read on next use. Identical immutable compiled
    // artifacts remain reusable; queued pipelines keep their shader lifetime.
    void invalidateInstructions() { instruction_epoch.fetch_add(1); }

    static void destroyElement(ShaderBase* shader);

    // The guest code a stage's last find() returned its shader for. While the
    // code (and the shader's branch constants) stay the same, the engine can
    // reuse that shader without a new lookup.
    struct FoundSource {
        ShaderBase* shader{nullptr};
        std::shared_ptr<const std::vector<u8>> bytes;
        std::shared_ptr<const std::vector<u8>> vertex_a_bytes;
    };
    const FoundSource& getLastFound(engines::ShaderStage stage) const {
        return last_found[static_cast<u32>(stage) % last_found.size()];
    }
    static bool matchesSource(const FoundSource& source, uptr gpu_addr,
                              uptr vertex_a_gpu_addr);
    // DEBUG: edited MSL replaces guest shaders (HYDRA_SHADER_OVERRIDE_DIR)
    static bool hasOverrides();

  private:
    IRenderer& renderer;
    std::array<FoundSource, 8> last_found;
    std::map<std::vector<u32>, std::map<std::pair<u32, u32>, u32>> branch_dependencies;
    struct SourceVersion {
        std::shared_ptr<const std::vector<u8>> bytes;
        u64 revision{0};
        u64 validated_epoch{0};
        std::vector<std::pair<std::shared_ptr<const std::vector<u8>>, u64>> history;
    };
    std::map<std::tuple<uptr, uptr, engines::ShaderStage>, SourceVersion> sources;
    struct CompiledVersion {
        std::shared_ptr<const std::vector<u8>> bytes;
        ShaderBase* shader;
        std::shared_ptr<const std::vector<u8>> vertex_a_bytes{};
    };
    std::map<std::vector<u32>, std::vector<CompiledVersion>> compiled_versions;
    // Reused under lookup_mutex; map entries own their copied keys.
    std::vector<u32> lookup_key;
    // The versions of the last key looked up per stage: consecutive draws
    // mostly use the same shaders. Entries of compiled_versions are never
    // erased, so the pointer stays valid.
    struct LookupMemo {
        std::vector<u32> key;
        std::vector<CompiledVersion>* versions{nullptr};
    };
    std::array<LookupMemo, 8> lookup_memos;
    u64 next_code_revision{0};
    std::atomic<u64> instruction_epoch{0};
    std::mutex lookup_mutex;

  public:
    // ABI-stable implementations selected by the live module.
    ShaderBase* findHot0(const GuestShaderDescriptor& descriptor);
    ShaderBase* createHot1(const GuestShaderDescriptor& descriptor);
    static u32 hashHot2(const GuestShaderDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer

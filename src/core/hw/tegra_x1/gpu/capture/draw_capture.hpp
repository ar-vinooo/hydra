#pragma once

#include "core/hw/tegra_x1/gpu/command_ir/clear.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/draw.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"

#include <atomic>
#include <limits>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Capture and replay of a short run of host-resolved commands (draws and
// clears) without the guest: the shaders as translated, the pipeline, pass
// and draw state, the bytes of every buffer, and the pixels of every texture
// the commands touch. Replaying them on a fresh renderer must produce the
// same pixels. The files are tied to the build that wrote them.

namespace hydra::hw::tegra_x1::gpu::capture {

constexpr char MAGIC[8] = {'H', 'Y', 'D', 'R', 'A', 'C', 'A', 'P'};
constexpr u32 VERSION = 3;

using Contents = std::vector<std::vector<u8>>;

struct TextureRecord {
    u32 type;
    u32 format;
    u32 width, height, depth, levels, layers;
    // Buffer textures: the byte size they are backed with
    bool is_linear;
    u32 linear_stride;
    // Memory layout of the guest copy below
    u32 block_width_gobs_log2, block_height_gobs_log2, block_depth_gobs_log2;
    u32 layer_size;
    // The guest memory behind the texture when it was first seen (2D
    // textures up to a size limit), to tell wrong guest data from wrong
    // host data
    std::vector<u8> guest;
    // Before the first captured command; empty if never read back
    Contents initial;
    // After the last one; empty if no command wrote the texture
    Contents final_contents;
};

struct ViewRecord {
    i32 texture;
    u32 type, format;
    u32 level_begin, level_end, layer_begin, layer_end;
    std::array<u8, 4> swizzle;
};

struct SamplerRecord {
    std::vector<u8> raw;
};

struct BufferRecord {
    std::vector<u8> bytes;
};

struct ShaderRecord {
    renderer::ShaderDescriptor descriptor;
};

struct PipelineRecord {
    i32 shaders[static_cast<usize>(renderer::ShaderType::Count)];
    std::vector<u8> vertex_state;
    std::vector<u8> color_targets;
    u32 depth_stencil_format;
    u32 topology_class;
};

struct TargetRecord {
    i32 view{-1};
    bool load_action_clear{false};
    std::array<u8, 16> clear_data{};
};

struct PassRecord {
    std::array<TargetRecord, COLOR_TARGET_COUNT> color;
    TargetRecord depth_stencil;
};

struct BufferRef {
    i32 buffer{-1};
    u64 offset{0};
    u64 size{0};
};

struct StageRecord {
    bool active{false};
    bool reset_textures{false};
    std::vector<std::pair<u32, BufferRef>> uniforms;
    std::vector<std::pair<u32, BufferRef>> storage;
    struct Texture {
        u32 index;
        i32 view;
        i32 sampler;
    };
    std::vector<Texture> textures;
};

struct DrawRecord {
    i32 pipeline{-1};
    i32 pass{-1};
    std::vector<u8> state;
    bool occlusion{false};
    std::array<BufferRef, VERTEX_ARRAY_COUNT> vertices;
    std::array<StageRecord, renderer::GRAPHICS_SHADER_TYPE_COUNT> stages;
    u32 primitive{0};
    u32 index_type{0};
    BufferRef indices;
    u32 first{0}, count{0}, base_vertex{0}, base_instance{0};
    u32 instance_count{1};
};

struct ClearRecord {
    i32 pass{-1};
    std::vector<u8> state;
    bool has_color{false}, has_depth{false}, has_stencil{false};
    u32 color_target{0}, color_layer{0}, color_mask{0};
    std::array<u32, 4> color_value{};
    u32 depth_layer{0};
    f32 depth_value{0};
    u32 stencil_layer{0}, stencil_value{0};
};

struct CommandRecord {
    enum class Kind : u32 { Draw, Clear } kind{Kind::Draw};
    DrawRecord draw;
    ClearRecord clear;
};

struct CaptureFile {
    std::vector<TextureRecord> textures;
    std::vector<ViewRecord> views;
    std::vector<SamplerRecord> samplers;
    std::vector<BufferRecord> buffers;
    std::vector<ShaderRecord> shaders;
    std::vector<PipelineRecord> pipelines;
    std::vector<PassRecord> passes;
    std::vector<CommandRecord> commands;
    // What the capture could not hold (a texture whose contents cannot be
    // read back is recorded without them); the replay may differ from the run
    std::vector<std::string> notes;

    std::vector<u8> serialize() const;
    // Throws std::runtime_error on a malformed or foreign file
    static CaptureFile deserialize(const std::vector<u8>& bytes);
    void save(const std::filesystem::path& path) const;
    static CaptureFile load(const std::filesystem::path& path);
};

// Collects commands as the engine executes them. Resources are recorded the
// first time they are seen, with the contents they have at that point, which
// is what replaying the earlier commands of the capture will recreate.
class Recorder {
  public:
    explicit Recorder(renderer::IRenderer& renderer_) : renderer{renderer_} {}

    // False if the command uses something captures do not cover
    bool recordDraw(renderer::ICommandBuffer* command_buffer,
                    const command_ir::DrawCommand& command);
    bool recordClear(renderer::ICommandBuffer* command_buffer,
                     const command_ir::ClearCommand& command);
    // Reads back what the commands wrote. Call once, after the last command
    // has been encoded.
    CaptureFile finish(renderer::ICommandBuffer* command_buffer);

    usize commandCount() const { return file.commands.size(); }
    // The capture stops growing once it holds this many bytes of contents
    void setLimit(u64 bytes) { limit_bytes = bytes; }
    // True once the limit was reached: later commands are not recorded, and
    // what was recorded so far is a complete capture of its own
    bool full() const { return reached_limit; }
    const std::string& failure() const { return error; }
    const std::vector<std::string>& notes() const { return file.notes; }

  private:
    renderer::IRenderer& renderer;
    CaptureFile file;
    std::string error;
    std::unordered_map<const void*, i32> textures, views, samplers, shaders,
        pipelines, passes;
    std::map<std::tuple<const void*, u64, u64, u64>, i32> buffers;
    std::vector<renderer::ITexture*> texture_objects;
    std::vector<bool> written;
    u64 capture_bytes{0};
    u64 limit_bytes{1ull << 30};
    bool reached_limit{false};

    i32 texture(renderer::ICommandBuffer* command_buffer,
                renderer::ITexture* object);
    i32 view(renderer::ICommandBuffer* command_buffer,
             renderer::ITextureView* object);
    i32 sampler(renderer::SamplerBase* object);
    i32 shader(renderer::ShaderBase* object);
    i32 pipeline(renderer::ICommandBuffer* command_buffer,
                 renderer::PipelineBase* object);
    i32 pass(renderer::ICommandBuffer* command_buffer,
             renderer::RenderPassBase* object, bool will_write);
    BufferRef buffer(const renderer::BufferView& view);
    bool fail(std::string message) {
        if (error.empty())
            error = std::move(message);
        return false;
    }
};

struct ReplayResult {
    bool ok{false};
    std::string error;
    // Per texture, the contents after replay (empty if never written)
    std::vector<Contents> contents;
    // Whether every texture the capture wrote has the captured pixels
    bool matches{false};
    std::string mismatch;
};

// Rebuilds the objects of the capture on `renderer`, runs its commands in
// order and reads back the textures they wrote. With a command limit only that
// many commands run (to find the first one whose result is wrong) and the
// textures are not compared with the captured ones.
// With a step, the textures the capture wrote are read back after every `step`
// commands and handed to the callback (after the command count that ran),
// starting once `first_step` commands ran.
using ReplayStepCallback =
    std::function<void(usize executed, const std::vector<Contents>& contents)>;
ReplayResult replay(renderer::IRenderer& renderer, const CaptureFile& file,
                    usize command_limit = std::numeric_limits<usize>::max(),
                    usize step = 0, const ReplayStepCallback& on_step = {},
                    usize first_step = 0);

// Live capture of the running engine: HYDRA_GPU_CAPTURE=<directory> enables
// it, and a file named `trigger` in that directory starts one. Its text may
// hold `commands=N` (default 2), `skip=N` (commands to let pass first, to reach
// a later part of a frame) and `limit_mb=N` (default 1024: the capture ends
// there, keeping the commands recorded so far). The capture is written next to it.
class LiveCapture {
  public:
    static LiveCapture& instance();

    bool active() const { return running.load(std::memory_order_acquire); }
    // Where captures go (HYDRA_GPU_CAPTURE sets it at startup; tests use this)
    void setDirectory(std::filesystem::path path) {
        std::scoped_lock lock(mutex);
        directory = std::move(path);
    }
    // Starts recording `commands` commands (what the trigger file does)
    void start(renderer::IRenderer& renderer, usize commands, usize skip = 0,
               u64 limit_bytes = 1ull << 30);
    // Polled once per presented frame
    void onPresent(renderer::IRenderer& renderer);
    // Called by the engines before they execute a command (a command beyond
    // the requested count is ignored)
    void draw(renderer::IRenderer& renderer,
              renderer::ICommandBuffer* command_buffer,
              const command_ir::DrawCommand& command);
    void clear(renderer::IRenderer& renderer,
               renderer::ICommandBuffer* command_buffer,
               const command_ir::ClearCommand& command);
    // Called after the command was encoded. When the requested number of
    // commands has been recorded this reads back what they wrote and writes
    // the file.
    void commandExecuted(renderer::ICommandBuffer* command_buffer);

  private:
    LiveCapture();
    void finishIfComplete(renderer::ICommandBuffer* command_buffer);
    // Reads back what was recorded and writes the file; ends the capture
    void complete(renderer::ICommandBuffer* command_buffer);

    std::filesystem::path directory;
    std::mutex mutex;
    std::atomic<bool> running{false};
    std::unique_ptr<Recorder> recorder;
    usize limit{0};
    u32 sequence{0};
    bool last_executed{false};
    usize to_skip{0};
};

} // namespace hydra::hw::tegra_x1::gpu::capture

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::hw::tegra_x1::gpu {

// Owned by hydra-core, including when callers run in a hot module.
class VisualDiagnostics {
  public:
    using Fields = std::map<std::string, std::string>;
    enum class Output : uint32_t { Guest, Solid, Uv, Normal, Checker };
    struct Options {
        std::optional<uint64_t> draw, shader, resource;
        std::string action{"capture"};
        Output output{Output::Guest};
        uint32_t input{0}, frames{2};
        bool no_depth{false}, no_cull{false}, no_blend{false};
        bool images{false}, shaders{false};
    };

    static VisualDiagnostics& instance();
    bool enabled() const { return !directory.empty(); }
    void onPresent();
    void submissionQueued(uint64_t id, uint64_t address_space, uint64_t entries);
    void beginSubmission(uint64_t id);
    void completed(uint64_t id, uint64_t epoch, std::string_view status,
                   std::string_view error, double gpu_seconds);
    void command(uint64_t address, uint32_t header);
    void commandFault(uint32_t method, uint32_t subchannel, uint32_t argument);
    uint64_t commandAddress() const { return command_address; }
    uint32_t commandHeader() const { return command_header; }
    bool beginDraw(uint64_t vs, uint64_t fs);
    void selectResource(uint64_t address);
    bool selected() const { return capturing && draw_selected; }
    bool skipDraw() const;
    bool active() const { return capturing; }
    void endDraw() { draw_selected = base_selected = false; }
    void beginShader(std::string key);
    void endShader();
    bool shaderActive() const { return !shader_key.empty(); }
    void shaderArtifact(std::string_view suffix, std::string_view data);
    void decoded(uint32_t pc, uint64_t word, std::string_view operation);
    static std::string contentId(std::span<const uint8_t> bytes);
    const Options& options() const { return config; }
    uint64_t drawIndex() const { return draw_index; }
    uint64_t submissionId() const { return submission_id; }
    uint64_t frameId() const { return frame; }
    void event(std::string_view type, const Fields& fields = {});
    std::string imagePath(std::string_view label);
    bool reserveImage(std::string_view path, uint64_t bytes);
    bool wantsShader(std::string_view key);
    void artifact(std::string_view name, std::string_view data);
    void artifact(std::string_view name, std::span<const uint8_t> data);
    static std::string quote(std::string_view text);

  private:
    VisualDiagnostics();
    void poll();
    void finish(std::string_view reason);
    void trackCompletion();
    void record(std::string_view type, const Fields& fields, uint64_t submission,
                uint64_t frame, uint64_t draw);
    std::filesystem::path directory, capture_directory;
    std::mutex mutex;
    std::deque<std::string> ring;
    std::ofstream stream;
    std::atomic<uint64_t> presents{0};
    uint64_t frame{0}, first_frame{0}, draw_index{UINT64_MAX}, submission_id{0};
    uint64_t command_address{0};
    uint32_t command_header{0}, before_images{0}, after_images{0}, texture_images{0}, event_count{0};
    size_t artifact_bytes{0};
    uint64_t image_bytes{0};
    uint32_t completion_count{0};
    std::map<uint64_t, std::vector<std::filesystem::path>> completion_paths;
    Options config;
    bool capturing{false}, draw_selected{false}, base_selected{false};
    std::set<std::string> shader_keys;
    std::string shader_key, decoded_text;
    std::chrono::steady_clock::time_point last_poll{}, deadline{};
};

} // namespace hydra::hw::tegra_x1::gpu

#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include <charconv>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <thread>

namespace hydra::hw::tegra_x1::gpu {
namespace {
uint64_t number(std::string_view value) {
    int base = 10;
    if (value.starts_with("0x")) {
        value.remove_prefix(2);
        base = 16;
    }
    uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid unsigned number");
    return result;
}
}

VisualDiagnostics& VisualDiagnostics::instance() {
    // Metal completion handlers can outlive System teardown.
    static auto* diagnostics = new VisualDiagnostics;
    return *diagnostics;
}

VisualDiagnostics::VisualDiagnostics() {
    if (const auto* env = std::getenv("HYDRA_VISUAL_DIR"))
        directory = env;
    else if (const auto* trace_dir = std::getenv("HYDRA_TRACE_DIR"))
        directory = std::filesystem::path(trace_dir) / "visual";
    if (!directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            std::cerr << "[VisualDiagnostics] " << ec.message() << '\n';
            directory.clear();
        }
    }
}

std::string VisualDiagnostics::quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const char character : text) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

void VisualDiagnostics::record(std::string_view type, const Fields& fields,
                               uint64_t submission, uint64_t frame_id, uint64_t draw) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string line = "{\"schema\":1,\"event\":" + quote(type) +
        ",\"time_ns\":" + std::to_string(ns) +
        ",\"submission\":" + std::to_string(submission) +
        ",\"present_epoch\":" + std::to_string(frame_id) +
        ",\"thread\":" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
        ",\"draw\":" + std::to_string(draw) + ",\"fields\":{";
    bool comma = false;
    for (const auto& [key, value] : fields) {
        if (comma) line += ',';
        line += quote(key) + ':' + quote(value);
        comma = true;
    }
    line += "}}\n";
    std::scoped_lock lock(mutex);
    ring.push_back(line);
    if (ring.size() > 256) ring.pop_front();
    const bool summary = type == "draw_begin" || type == "draw_pipeline" ||
                         type == "cpu_submit" || type == "gpu_submit" || type == "gpu_completed";
    if (stream.is_open() && (type == "capture_end" ||
        (event_count < 4096 && (!summary || event_count < 512)))) {
        stream << line;
        ++event_count;
        if (event_count == 4096)
            stream << "{\"schema\":1,\"event\":\"event_budget_exhausted\"}\n";
        if (!stream)
            std::cerr << "[VisualDiagnostics] event write failed\n";
    }
}

void VisualDiagnostics::onPresent() {
    if (enabled()) ++presents;
}

void VisualDiagnostics::submissionQueued(uint64_t id, uint64_t address_space, uint64_t entries) {
    if (enabled())
        record("cpu_submit", {{"address_space", std::to_string(address_space)},
                               {"entries", std::to_string(entries)}}, id, presents.load(), 0);
}

void VisualDiagnostics::beginSubmission(uint64_t id) {
    if (!enabled()) return;
    submission_id = id;
    poll();
    trackCompletion();
    event("gpu_submit");
}

void VisualDiagnostics::completed(uint64_t id, uint64_t epoch, std::string_view status,
                                  std::string_view error, double gpu_seconds) {
    if (!enabled()) return;
    record("gpu_completed", {{"status", std::string(status)},
        {"error", std::string(error)}, {"gpu_seconds", std::to_string(gpu_seconds)}}, id, epoch, 0);
    std::scoped_lock lock(mutex);
    if (const auto it = completion_paths.find(id); it != completion_paths.end()) {
        for (const auto& path : it->second) {
            std::ofstream out(path / "completions.jsonl", std::ios::app);
            out << "{\"schema\":1,\"event\":\"gpu_completed\",\"submission\":" << id
                << ",\"present_epoch\":" << epoch << ",\"fields\":{\"status\":" << quote(status)
                << ",\"error\":" << quote(error) << ",\"gpu_seconds\":" << quote(std::to_string(gpu_seconds)) << "}}\n";
            if (!out) std::cerr << "[VisualDiagnostics] completion write failed\n";
        }
        completion_paths.erase(it);
    }
}

void VisualDiagnostics::trackCompletion() {
    if (!capturing) return;
    std::scoped_lock lock(mutex);
    if (completion_count >= 128) return;
    if (!completion_paths.contains(submission_id) && completion_paths.size() >= 128)
        completion_paths.erase(completion_paths.begin());
    auto& paths = completion_paths[submission_id];
    if (std::find(paths.begin(), paths.end(), capture_directory) != paths.end() || paths.size() >= 8) return;
    paths.push_back(capture_directory);
    ++completion_count;
}

void VisualDiagnostics::command(uint64_t address, uint32_t header) {
    if (!enabled()) return;
    command_address = address;
    command_header = header;
}

void VisualDiagnostics::event(std::string_view type, const Fields& fields) {
    if (enabled()) record(type, fields, submission_id, frame, draw_index);
}

void VisualDiagnostics::finish(std::string_view reason) {
    if (!capturing) return;
    event("capture_end", {{"reason", std::string(reason)},
                           {"artifacts_bytes", std::to_string(artifact_bytes)}});
    std::scoped_lock lock(mutex);
    stream.close();
    stream.clear();
    capturing = draw_selected = base_selected = false;
    config = {};
}

void VisualDiagnostics::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (capturing && (presents.load() >= first_frame + config.frames || now >= deadline))
        finish("expired_overrides_restored");
    if (now - last_poll < std::chrono::milliseconds(100)) return;
    last_poll = now;
    std::error_code ec;
    const auto request = directory / "request";
    if (!std::filesystem::exists(request, ec)) return;
    if (std::filesystem::file_size(request, ec) > 4096 || ec) {
        std::filesystem::remove(request, ec);
        event("request_rejected", {{"reason", "request exceeds 4096 bytes or cannot be read"}});
        return;
    }
    std::ifstream input(request);
    std::string token, request_text;
    Options next;
    try {
        if (!input) throw std::runtime_error("cannot read request");
        while (input >> token) {
            request_text += token + " ";
            const auto split = token.find('=');
            if (split == std::string::npos) throw std::runtime_error("expected key=value");
            const auto key = token.substr(0, split), value = token.substr(split + 1);
            if (key == "action") next.action = value;
            else if (key == "draw") next.draw = number(value);
            else if (key == "shader") next.shader = number(value);
            else if (key == "resource") next.resource = number(value);
            else if (key == "frames") {
                const auto n = number(value);
                if (n < 1 || n > 8) throw std::runtime_error("frames must be 1..8");
                next.frames = static_cast<uint32_t>(n);
            } else if (key == "input") {
                const auto n = number(value);
                if (n > 31) throw std::runtime_error("input must be 0..31");
                next.input = static_cast<uint32_t>(n);
            } else if (key == "output") {
                if (value == "guest") next.output = Output::Guest;
                else if (value == "solid") next.output = Output::Solid;
                else if (value == "uv") next.output = Output::Uv;
                else if (value == "normal") next.output = Output::Normal;
                else if (value == "checker") next.output = Output::Checker;
                else throw std::runtime_error("unknown output");
            } else if (key == "images" || key == "shaders" || key == "no_depth" ||
                       key == "no_cull" || key == "no_blend") {
                if (value != "0" && value != "1") throw std::runtime_error("expected boolean 0 or 1");
                const bool b = value == "1";
                if (key == "images") next.images = b;
                if (key == "shaders") next.shaders = b;
                if (key == "no_depth") next.no_depth = b;
                if (key == "no_cull") next.no_cull = b;
                if (key == "no_blend") next.no_blend = b;
            } else throw std::runtime_error("unknown option: " + key);
        }
        if (next.action != "capture" && next.action != "skip" && next.action != "only" && next.action != "reset")
            throw std::runtime_error("unknown action");
        if ((next.action == "skip" || next.action == "only" || next.output != Output::Guest ||
             next.no_depth || next.no_cull || next.no_blend) &&
            !next.draw && !next.shader && !next.resource)
            throw std::runtime_error("override requires an explicit selector");
        if (next.resource && (next.output != Output::Guest || next.no_blend))
            throw std::runtime_error("resource selector supports capture/skip/only/depth/cull; select draw or shader for shader/blend overrides");
        const auto interventions = (next.action == "skip" || next.action == "only" ? 1 : 0) +
            (next.output != Output::Guest ? 1 : 0) + int(next.no_depth) + int(next.no_cull) + int(next.no_blend);
        if (interventions > 1) throw std::runtime_error("use one diagnostic intervention per capture");
        finish("replaced_overrides_restored");
        std::filesystem::remove(request, ec);
        if (next.action == "reset") return;
        // Unique immutable output folders: callbacks from old captures never overwrite new images.
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        capture_directory = directory / ("capture-" + std::to_string(stamp));
        std::filesystem::create_directories(capture_directory, ec);
        if (ec) throw std::runtime_error(ec.message());
        {
            std::scoped_lock lock(mutex);
            stream.clear();
            stream.open(capture_directory / "events.jsonl");
            if (!stream) throw std::runtime_error("cannot open events.jsonl");
            for (const auto& line : ring) stream << line;
            event_count = 0;
        }
        config = next;
        first_frame = presents.load();
        deadline = now + std::chrono::seconds(10);
        artifact_bytes = 0;
        before_images = after_images = texture_images = 0;
        image_bytes = 0;
        completion_count = 0;
        shader_keys.clear();
        capturing = true;
        event("capture_begin", {{"request", request_text}, {"action", config.action},
            {"output", std::to_string(static_cast<uint32_t>(config.output))},
            {"input", std::to_string(config.input)}, {"frames", std::to_string(config.frames)},
            {"no_depth", std::to_string(config.no_depth)}, {"no_cull", std::to_string(config.no_cull)},
            {"no_blend", std::to_string(config.no_blend)}});
    } catch (const std::exception& error) {
        std::filesystem::remove(request, ec);
        event("request_rejected", {{"reason", error.what()}});
        std::cerr << "[VisualDiagnostics] request rejected: " << error.what() << '\n';
    }
}

bool VisualDiagnostics::beginDraw(uint64_t vs, uint64_t fs) {
    if (!enabled()) return false;
    poll();
    trackCompletion();
    const auto epoch = presents.load();
    if (frame != epoch) {
        frame = epoch;
        draw_index = 0;
    } else ++draw_index;
    base_selected = capturing && (config.draw || config.shader || config.resource) && (!config.draw || *config.draw == draw_index) &&
        (!config.shader || *config.shader == vs || *config.shader == fs);
    draw_selected = base_selected && !config.resource;
    if (capturing)
        event("draw_begin", {{"vs", std::to_string(vs)}, {"fs", std::to_string(fs)},
            {"command_address", std::to_string(command_address)},
            {"command_header", std::to_string(command_header)}});
    return selected();
}

void VisualDiagnostics::selectResource(uint64_t address) {
    if (base_selected && config.resource && *config.resource == address)
        draw_selected = true;
}

bool VisualDiagnostics::skipDraw() const {
    return capturing && ((config.action == "skip" && selected()) ||
                         (config.action == "only" && !selected()));
}

std::string VisualDiagnostics::imagePath(std::string_view label) {
    if (!selected() || !config.images) return {};
    auto& count = label.starts_with("before-") ? before_images :
                  label.starts_with("after-") ? after_images : texture_images;
    const uint32_t limit = label.starts_with("after-") ? 8u : 4u;
    if (count >= limit) return {};
    ++count;
    return (capture_directory / ("p" + std::to_string(frame) + "-d" +
        std::to_string(draw_index) + "-" + std::string(label) + ".png")).string();
}

bool VisualDiagnostics::reserveImage(std::string_view path, uint64_t bytes) {
    if (!capturing || !path.starts_with(capture_directory.string() + "/")) return true;
    if (bytes > 8 * 1024 * 1024 || image_bytes + bytes > 64 * 1024 * 1024) {
        event("image_omitted", {{"path", std::string(path)}, {"reason", "readback_byte_budget"}});
        return false;
    }
    image_bytes += bytes;
    return true;
}

bool VisualDiagnostics::wantsShader(std::string_view key) {
    return selected() && config.shaders && shader_keys.size() < 16 &&
        shader_keys.insert(std::string(key)).second;
}

void VisualDiagnostics::artifact(std::string_view name, std::string_view data) {
    static thread_local std::filesystem::path counted_directory;
    static thread_local uint32_t artifact_count = 0;
    if (counted_directory != capture_directory) {
        counted_directory = capture_directory;
        artifact_count = 0;
    }
    if (capturing && artifact_count >= 512) {
        if (artifact_count == 512) {
            event("artifact_omitted", {{"reason", "file_count_budget_512"}});
            ++artifact_count;
        }
        return;
    }
    if (!capturing || data.size() > 4 * 1024 * 1024 ||
        artifact_bytes + data.size() > 16 * 1024 * 1024) {
        if (capturing) event("artifact_omitted", {{"name", std::string(name)}, {"reason", "byte_budget"}});
        return;
    }
    ++artifact_count;
    std::ofstream out(capture_directory / std::string(name), std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) event("artifact_error", {{"name", std::string(name)}});
    else {
        artifact_bytes += data.size();
        event("artifact", {{"name", std::string(name)}, {"bytes", std::to_string(data.size())}});
    }
}

void VisualDiagnostics::artifact(std::string_view name, std::span<const uint8_t> data) {
    artifact(name, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::string VisualDiagnostics::contentId(std::span<const uint8_t> bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    return std::to_string(hash);
}

void VisualDiagnostics::beginShader(std::string key) {
    shader_key = std::move(key);
    decoded_text.clear();
}

void VisualDiagnostics::endShader() {
    if (!shader_key.empty()) shaderArtifact("decoded.txt", decoded_text);
    shader_key.clear();
    decoded_text.clear();
}

void VisualDiagnostics::shaderArtifact(std::string_view suffix, std::string_view data) {
    if (shaderActive()) artifact(shader_key + "." + std::string(suffix), data);
}

void VisualDiagnostics::decoded(uint32_t pc, uint64_t word, std::string_view operation) {
    if (!shaderActive()) return;
    if (decoded_text.size() > 1024 * 1024) {
        if (!decoded_text.ends_with("TRUNCATED\n")) decoded_text += "TRUNCATED\n";
        return;
    }
    decoded_text += std::to_string(pc) + " raw=" + std::to_string(word) +
                    " op=" + std::string(operation) + "\n";
}

} // namespace hydra::hw::tegra_x1::gpu

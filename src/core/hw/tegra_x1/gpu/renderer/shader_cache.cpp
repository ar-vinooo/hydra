#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#include "core/hot/table.hpp"

#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <chrono>
#include <set>
#include <mutex>
#include <filesystem>

namespace hydra::hw::tegra_x1::gpu::renderer {

namespace {

// DEBUG: replace guest shaders with edited MSL while the game runs.
// HYDRA_SHADER_OVERRIDE_DIR holds "<gpu addr>_<type>.metal" files, and creating
// "<dir>/reload" recompiles the overridden shaders from them.
struct ShaderOverrides {
    std::filesystem::path dir;
    std::mutex mutex;
    std::set<u64> addrs; // Every address that had an override
    std::atomic<bool> any{false};
    u32 generation{0};
    u32 poll_calls{0};

    static ShaderOverrides& instance() {
        static ShaderOverrides overrides;
        return overrides;
    }

    ShaderOverrides() {
        if (const char* env = std::getenv("HYDRA_SHADER_OVERRIDE_DIR"))
            dir = env;
#ifdef HYDRA_DEFAULT_HOT_DIR
        else
            dir = std::filesystem::path(HYDRA_DEFAULT_HOT_DIR) / "override";
#endif
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            scan();
        }
    }

    void scan() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() == ".metal")
                addrs.insert(std::strtoull(name.c_str(), nullptr, 0));
        }
        any = !addrs.empty();
    }

    void poll() {
        if (dir.empty())
            return;
        // Checking the file system (or even the clock) for every draw is too
        // slow
        if ((poll_calls++ & 255) != 0)
            return;
        static auto last_check = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check < std::chrono::milliseconds(500))
            return;
        last_check = now;

        std::error_code ec;
        if (!std::filesystem::remove(dir / "reload", ec))
            return;
        std::scoped_lock lock(mutex);
        scan();
        generation++;
        LOG_INFO(ShaderDecompiler, "Reloading shader overrides (generation {})",
                 generation);
    }

    u32 getGeneration(u64 addr) {
        if (dir.empty() || !any.load(std::memory_order_relaxed))
            return 0;
        std::scoped_lock lock(mutex);
        return addrs.contains(addr) ? generation : 0;
    }

    std::optional<std::string> read(u64 addr, ShaderType type) {
        if (dir.empty())
            return std::nullopt;
        std::ifstream in(dir / fmt::format("{:#x}_{}.metal", addr, type));
        if (!in)
            return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};

} // namespace

ShaderBase* ShaderCache::findHot0(const GuestShaderDescriptor& descriptor) {
    ShaderOverrides::instance().poll();
    const auto inspect = [&](const GuestShaderDescriptor& actual, ShaderBase* shader) {
        auto& visual = VisualDiagnostics::instance();
        if (!visual.selected()) return shader;
        const auto key = fmt::format("shader-{:x}-{}-{:x}-r{}", actual.gpu_addr,
            actual.stage, hash(actual), ::hydra::hot::generation());
        visual.event("shader_cache", {{"identity", key},
            {"guest_address", std::to_string(actual.gpu_addr)},
            {"stage", fmt::format("{}", actual.stage)},
            {"shader_present", shader ? "1" : "0"}});
        if (!shader || !visual.wantsShader(key)) return shader;
        auto code = tls_crnt_gmmu->readMappedBytes(actual.gpu_addr, 0x10000);
        visual.artifact(key + ".guest.bin", std::span<const uint8_t>(code));
        visual.artifact(key + ".bound.metal", std::span<const uint8_t>(shader->getDescriptor().code));
        visual.event("shader_snapshot", {{"identity", key},
            {"guest_fnv1a64", VisualDiagnostics::contentId(code)},
            {"bound_source_fnv1a64", VisualDiagnostics::contentId(shader->getDescriptor().code)},
            {"decoder", "diagnostic_replay_of_current_guest_bytes"}});
        if (code.size() < (actual.stage == engines::ShaderStage::Compute ? 8u : 88u)) {
            visual.event("shader_snapshot_incomplete", {{"identity", key}});
            return shader;
        }
        visual.beginShader(key);
        struct EndShader {
            VisualDiagnostics& visual;
            ~EndShader() { visual.endShader(); }
        } end_shader{visual};
        try {
            ztd::io::MemoryStream stream(code);
            ShaderDescriptor replay{};
            replay.type = engines::toRendererShaderType(actual.stage);
            shader_decomp::decompile(stream, replay.type, actual.state,
                replay.backend, replay.code, replay.resource_mapping, actual.read_const_word);
            visual.artifact(key + ".replay.metal", std::span<const uint8_t>(replay.code));
            visual.event("shader_replay", {{"identity", key},
                {"matches_bound_source", replay.code == shader->getDescriptor().code ? "1" : "0"},
                {"note", "difference can indicate an MSL override, changed guest code, or changed specialization; compilation is not semantic validation"}});
        } catch (const std::exception& error) {
            visual.event("shader_replay_failed", {{"identity", key}, {"error", error.what()}});
        }
        return shader;
    };
    const auto base_hash = hash(descriptor);
    // Most shaders have no constant-buffer jump table. Reuse the hash and
    // avoid copying all resource maps just to perform the same lookup again.
    const auto dependency = branch_dependencies.find(base_hash);
    if (dependency == branch_dependencies.end() || dependency->second.empty()) {
        auto it = cache.find(base_hash);
        if (it == cache.end())
            it = cache.emplace(base_hash, create(descriptor)).first;
        return inspect(descriptor, it->second);
    }
    auto actual = descriptor;
    if (const auto it = branch_dependencies.find(base_hash);
        it != branch_dependencies.end()) {
        for (const auto& [location, previous] : it->second) {
            const auto value = descriptor.read_const_word
                                   ? descriptor.read_const_word(location.first,
                                                                location.second)
                                   : std::nullopt;
            (void)previous;
            if (!value) {
                LOG_ERROR(ShaderDecompiler, "Missing branch table c{}[{}]",
                          location.first, location.second);
                return nullptr;
            }
            actual.state.branch_constants[location] = *value;
        }
    }
    return inspect(actual, CacheBase<ShaderCache, ShaderBase*, GuestShaderDescriptor>::find(actual));
}

ShaderBase* ShaderCache::createHot1(const GuestShaderDescriptor& descriptor) {
    ShaderDescriptor host_descriptor{};
    host_descriptor.type = engines::toRendererShaderType(descriptor.stage);
    host_descriptor.writes_layer =
        descriptor.stage == engines::ShaderStage::VertexB &&
        descriptor.state.layer_output_addr != 0;

    // Decompile
    // Gameplay shaders can exceed 4 KiB, and guest-contiguous code may span
    // physically separate pages. Decode an owned, bounded snapshot.
    auto code = tls_crnt_gmmu->readMappedBytes(descriptor.gpu_addr, 0x10000);
    const usize minimum_size =
        descriptor.stage == engines::ShaderStage::Compute ? 8 : 80 + 8;
    if (code.size() < minimum_size) {
        LOG_ERROR(ShaderDecompiler, "Shader at {:#x} has only {} mapped bytes",
                  descriptor.gpu_addr, code.size());
        return nullptr;
    }
    if (const char* dump_dir = std::getenv("HYDRA_DUMP_SHADERS")) {
        static std::atomic<u32> input_index{0};
        std::ofstream out(
            fmt::format("{}/gpu_{:#x}_{}_{:04}.bin", dump_dir,
                        descriptor.gpu_addr, host_descriptor.type,
                        input_index++),
            std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()),
                  static_cast<std::streamsize>(code.size()));
    }
    ztd::io::MemoryStream code_stream(code);
    shader_decomp::decompile(
        code_stream, host_descriptor.type, descriptor.state,
        host_descriptor.backend, host_descriptor.code,
        host_descriptor.resource_mapping, descriptor.read_const_word);
    host_descriptor.writes_layer |= host_descriptor.resource_mapping.writes_layer;
    if (const char* dump_dir = std::getenv("HYDRA_DUMP_SHADERS")) {
        std::ofstream out(fmt::format("{}/gpu_{:#x}_{}.metal", dump_dir,
                                      descriptor.gpu_addr,
                                      host_descriptor.type),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(host_descriptor.code.data()),
                  static_cast<std::streamsize>(host_descriptor.code.size()));
    }
    if (const auto source = ShaderOverrides::instance().read(
            descriptor.gpu_addr, host_descriptor.type)) {
        LOG_INFO(ShaderDecompiler, "Overriding shader {:#x}", descriptor.gpu_addr);
        host_descriptor.code.assign(source->begin(), source->end());
    }
    auto base_descriptor = descriptor;
    base_descriptor.state.branch_constants.clear();
    branch_dependencies[hash(base_descriptor)] =
        host_descriptor.resource_mapping.branch_constants;
    return renderer.createShader(host_descriptor);
}

u32 ShaderCache::hashHot2(const GuestShaderDescriptor& descriptor) {
    // Hash enum values as words rather than feeding each byte separately.
    // Keep explicit fields: hashing whole structs would include padding.
    ztd::hash::XxHash32 hash;
    hash.add(static_cast<u32>(descriptor.stage));
    hash.add(descriptor.code_ptr);
    hash.add(descriptor.state.program_offset);
    hash.add(ShaderOverrides::instance().getGeneration(descriptor.gpu_addr));
    for (const auto& [location, value] : descriptor.state.branch_constants) {
        hash.add(location.first);
        hash.add(location.second);
        hash.add(value);
    }
    for (const auto& [index, format] :
         descriptor.state.buffer_texture_formats) {
        hash.add(index);
        hash.add(static_cast<u32>(format));
    }
    for (const auto& [index, format] :
         descriptor.state.integer_texture_formats) {
        hash.add(index | 0x80000000u);
        hash.add(static_cast<u32>(format));
    }
    if (descriptor.stage == engines::ShaderStage::Compute) {
        hash.add(descriptor.state.shared_memory_size);
        for (const auto& [index, format] : descriptor.state.image_formats) {
            hash.add(index);
            hash.add(static_cast<u32>(format));
        }
    }

    // Take a few samples from the code (one mapping lookup when they are all
    // in the same mapping, which is almost always the case)
    {
        uptr base = 0;
        std::optional<AddressSpace> as;
        for (usize offset = 80; offset < 224; offset += 18) {
            const auto addr = descriptor.gpu_addr + offset;
            if (!as || addr - base >= as->size) {
                as = tls_crnt_gmmu->findAddr(addr, base);
                if (!as || as->ptr == 0)
                    break;
            }
            hash.add(*reinterpret_cast<const u8*>(as->ptr + addr - base));
        }
    }

    for (const auto& [index, bias] : descriptor.state.texture_lod_bias) {
        hash.add(index);
        hash.add(bias);
    }

    for (const auto& [index, sampler] : descriptor.state.border_samplers) {
        hash.add(index);
        hash.add(sampler.min_filter);
        hash.add(sampler.mag_filter);
        hash.add(sampler.mip_filter);
        hash.add(sampler.address_mode_s);
        hash.add(sampler.address_mode_t);
        hash.add(sampler.address_mode_r);
        hash.add(sampler.depth_compare_op);
        hash.add(sampler.border_color_u);
        hash.add(sampler.min_lod);
        hash.add(sampler.max_lod);
        hash.add(sampler.max_anisotropy);
        hash.add(sampler.integer_texture);
    }

    hash.add(descriptor.state.unnormalized_textures.size());
    for (const auto index : descriptor.state.unnormalized_textures)
        hash.add(index);

    // Vertex state
    if (descriptor.stage == engines::ShaderStage::VertexB) {
        for (const auto& vertex_attrib_state :
             descriptor.state.vertex_attrib_states) {
            hash.add(vertex_attrib_state.is_fixed);
            hash.add(static_cast<u32>(vertex_attrib_state.size));
            hash.add(static_cast<u32>(vertex_attrib_state.type));
            hash.add(vertex_attrib_state.bgra);
        }
        hash.add(descriptor.state.fragment_input_mask);
        hash.add(descriptor.state.depth_zero_to_one);
        hash.add(descriptor.state.layer_output_addr);
        hash.add(descriptor.state.clip_distance_mask);
        hash.add(descriptor.state.point_list);
        if (descriptor.state.point_list) {
            hash.add(descriptor.state.program_point_size);
            hash.add(std::bit_cast<u32>(descriptor.state.point_size));
        }
    }
    hash.add(descriptor.state.y_negate);
    hash.add(descriptor.state.window_space_width);
    hash.add(descriptor.state.window_space_height);
    if (descriptor.stage == engines::ShaderStage::Fragment) {
        hash.add(descriptor.state.point_coord_origin_bottom);
        hash.add(descriptor.state.alpha_test_func);
        hash.add(std::bit_cast<u32>(descriptor.state.alpha_test_ref));
        hash.add(descriptor.state.dual_source_blending);
    }

    // Color target data types
    if (descriptor.stage == engines::ShaderStage::Fragment) {
        hash.add(descriptor.state.debug_highlight);
        hash.add(descriptor.state.visual_output);
        hash.add(descriptor.state.visual_input);
        for (const auto& color_target_data_type :
             descriptor.state.color_target_data_types) {
            hash.add(static_cast<u32>(color_target_data_type));
        }
    }

    return hash.toHashCode();
}

void ShaderCache::destroyElement(ShaderBase* shader) { delete shader; }

} // namespace hydra::hw::tegra_x1::gpu::renderer

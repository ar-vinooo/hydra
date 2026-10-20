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

// The same explicit fields form the hash and the exact collision key. Do not
// hash struct padding or treat a 32-bit hash as proof of descriptor equality.
struct ShaderKeyBuilder {
    std::vector<u32> words;
    ShaderKeyBuilder() { words.reserve(128); }
    void add(u32 value) { words.push_back(value); }
    void add(u64 value) { add(u32(value)); add(u32(value >> 32)); }
    template <typename T> void add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        for (usize i = 0; i < sizeof(T); ++i) add(u32(bytes[i]));
    }
};

bool matchesMappedCode(uptr address, const std::vector<u8>& bytes,
                       u64 prefix = 0) {
    const usize count_to_check = prefix != 0
        ? static_cast<usize>(prefix) : bytes.size();
    if (count_to_check > bytes.size()) return false;
    usize offset = 0;
    while (offset < count_to_check) {
        uptr base;
        const auto mapping = tls_crnt_gmmu->findAddr(address + offset, base);
        if (!mapping || mapping->ptr == 0) return false;
        const auto within = address + offset - base;
        const auto count = std::min<usize>(mapping->size - within, count_to_check - offset);
        if (std::memcmp(bytes.data() + offset,
                        reinterpret_cast<const void*>(mapping->ptr + within), count) != 0)
            return false;
        offset += count;
    }
    if (prefix != 0 || bytes.size() == 0x10000) return true;
    // A previously unmapped tail becoming mapped also changes the snapshot.
    uptr base;
    const auto tail = tls_crnt_gmmu->findAddr(address + bytes.size(), base);
    return !tail || tail->ptr == 0;
}

std::vector<u32> makeShaderKey(const GuestShaderDescriptor& descriptor) {
    // Hash enum values as words rather than feeding each byte separately.
    // Keep explicit fields: hashing whole structs would include padding.
    ShaderKeyBuilder hash;
    hash.add(descriptor.gpu_addr);
    hash.add(reinterpret_cast<uptr>(tls_crnt_gmmu));
    hash.add(::hydra::hot::generation());
    hash.add(descriptor.code_revision);
    hash.add(static_cast<u32>(descriptor.stage));
    hash.add(descriptor.code_ptr);
    hash.add(descriptor.state.program_offset);
    hash.add(ShaderOverrides::instance().getGeneration(descriptor.gpu_addr));
    hash.add(descriptor.state.branch_constants.size());
    for (const auto& [location, value] : descriptor.state.branch_constants) {
        hash.add(location.first);
        hash.add(location.second);
        hash.add(value);
    }
    hash.add(descriptor.state.buffer_texture_formats.size());
    for (const auto& [index, format] :
         descriptor.state.buffer_texture_formats) {
        hash.add(index);
        hash.add(static_cast<u32>(format));
    }
    hash.add(descriptor.state.integer_texture_formats.size());
    for (const auto& [index, format] :
         descriptor.state.integer_texture_formats) {
        hash.add(index | 0x80000000u);
        hash.add(static_cast<u32>(format));
    }
    if (descriptor.stage == engines::ShaderStage::Compute) {
        hash.add(descriptor.state.shared_memory_size);
        hash.add(descriptor.state.image_formats.size());
        for (const auto& [index, format] : descriptor.state.image_formats) {
            hash.add(index);
            hash.add(static_cast<u32>(format));
        }
    }

    hash.add(descriptor.state.texture_lod_bias.size());
    for (const auto& [index, bias] : descriptor.state.texture_lod_bias) {
        hash.add(index);
        hash.add(bias);
    }

    hash.add(descriptor.state.texture_type_hints.size());
    for (const auto& [index, type] : descriptor.state.texture_type_hints) {
        hash.add(index | 0x40000000u);
        hash.add(static_cast<u32>(type));
    }

    hash.add(descriptor.state.border_samplers.size());
    for (const auto& [index, sampler] : descriptor.state.border_samplers) {
        hash.add(index);
        hash.add(sampler.min_filter);
        hash.add(sampler.mag_filter);
        hash.add(sampler.mip_filter);
        hash.add(sampler.address_mode_s);
        hash.add(sampler.address_mode_t);
        hash.add(sampler.address_mode_r);
        hash.add(sampler.mirror_to_border_mask);
        hash.add(sampler.depth_compare_op);
        hash.add(sampler.border_color_u);
        hash.add(sampler.min_lod);
        hash.add(sampler.max_lod);
        hash.add(sampler.max_anisotropy);
        hash.add(sampler.integer_texture);
    }

    // DEBUG: isolation switch of the emitter
    static const bool neutral_3d =
        std::getenv("HYDRA_DEBUG_NEUTRAL_3D") != nullptr;
    hash.add(neutral_3d);

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
        for (const auto swizzle : descriptor.state.color_target_swizzles)
            hash.add(static_cast<u32>(swizzle));
    }

    return std::move(hash.words);
}


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
        auto code = actual.code_snapshot ? *actual.code_snapshot
            : tls_crnt_gmmu->readMappedBytes(actual.gpu_addr, 0x10000);
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
    std::scoped_lock lock(lookup_mutex);
    auto actual = descriptor;
    actual.code_revision = 0;
    actual.state.branch_constants.clear();
    const auto version_key = makeShaderKey(actual);
    auto& versions = compiled_versions[version_key];
    for (const auto& version : versions) {
        const auto& resources = version.shader->getDescriptor().resource_mapping;
        if (!matchesMappedCode(descriptor.gpu_addr, *version.bytes,
                               resources.code_read_size))
            continue;
        const auto constants_match = std::all_of(
            resources.branch_constants.begin(), resources.branch_constants.end(),
            [&](const auto& entry) {
                return descriptor.read_const_word &&
                    descriptor.read_const_word(entry.first.first,
                                               entry.first.second) == entry.second;
            });
        if (constants_match) return inspect(actual, version.shader);
    }
    auto& source = sources[{reinterpret_cast<uptr>(tls_crnt_gmmu), descriptor.gpu_addr,
                            descriptor.stage}];
    const auto epoch = instruction_epoch.load();
    if (!source.bytes || source.validated_epoch != epoch ||
        !matchesMappedCode(descriptor.gpu_addr, *source.bytes)) {
        auto bytes = tls_crnt_gmmu->readMappedBytes(descriptor.gpu_addr, 0x10000);
        const usize minimum = descriptor.stage == engines::ShaderStage::Compute ? 8 : 88;
        if (bytes.size() < minimum) return nullptr;
        // A -> B -> A code uploads should reuse A's immutable artifact rather
        // than accumulating duplicate shader/pipeline objects every frame.
        const auto known = std::find_if(source.history.begin(), source.history.end(),
            [&](const auto& version) { return *version.first == bytes; });
        if (known != source.history.end()) {
            source.bytes = known->first;
            source.revision = known->second;
        } else {
            source.bytes = std::make_shared<const std::vector<u8>>(std::move(bytes));
            source.revision = ++next_code_revision;
            source.history.emplace_back(source.bytes, source.revision);
        }
        source.validated_epoch = epoch;
    }
    actual.code_snapshot = source.bytes;
    actual.code_revision = source.revision;
    actual.state.branch_constants.clear();
    actual.cache_key = makeShaderKey(actual);
    if (const auto it = branch_dependencies.find(actual.cache_key);
        it != branch_dependencies.end() && !it->second.empty()) {
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
        actual.cache_key = makeShaderKey(actual);
    }
    auto* shader = CacheBase<ShaderCache, ShaderBase*, GuestShaderDescriptor, true, true>::find(actual);
    if (shader) versions.push_back({actual.code_snapshot, shader});
    return inspect(actual, shader);
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
    auto code = descriptor.code_snapshot ? *descriptor.code_snapshot
        : tls_crnt_gmmu->readMappedBytes(descriptor.gpu_addr, 0x10000);
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
    if (!host_descriptor.resource_mapping.unsupported.empty()) {
        std::string features;
        for (const auto& feature : host_descriptor.resource_mapping.unsupported)
            features += (features.empty() ? "" : "; ") + feature;
        LOG_WARN(ShaderDecompiler,
                 "Shader {:#x} ({}) uses unsupported features and may "
                 "compute wrong values: {}",
                 descriptor.gpu_addr, host_descriptor.type, features);
    }
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
    branch_dependencies[makeShaderKey(base_descriptor)] =
        host_descriptor.resource_mapping.branch_constants;
    return renderer.createShader(host_descriptor);
}

u32 ShaderCache::hashHot2(const GuestShaderDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    if (!descriptor.cache_key.empty()) {
        for (const auto word : descriptor.cache_key) hash.add(word);
    } else {
        for (const auto word : makeShaderKey(descriptor)) hash.add(word);
    }
    if (descriptor.cache_key.empty()) {
        // Direct callers of hash() also see every mapped byte, including the
        // graphics header, compute entry and code beyond the old sample range.
        auto code = descriptor.code_snapshot ? *descriptor.code_snapshot
            : tls_crnt_gmmu->readMappedBytes(descriptor.gpu_addr, 0x10000);
        hash.add(code.size());
        usize offset = 0;
        for (; offset + 4 <= code.size(); offset += 4) {
            u32 word;
            std::memcpy(&word, code.data() + offset, 4);
            hash.add(word);
        }
        for (; offset < code.size(); ++offset) hash.add(u32(code[offset]));
    }
    return hash.toHashCode();
}

bool ShaderCache::equals(const GuestShaderDescriptor& a,
                         const GuestShaderDescriptor& b) {
    return a.cache_key == b.cache_key &&
           (a.code_snapshot == b.code_snapshot ||
            (a.code_snapshot && b.code_snapshot && *a.code_snapshot == *b.code_snapshot));
}

void ShaderCache::destroyElement(ShaderBase* shader) { delete shader; }

} // namespace hydra::hw::tegra_x1::gpu::renderer

#include "core/hw/tegra_x1/gpu/capture/draw_capture.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/compute_limits.hpp"

#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hydra::hw::tegra_x1::gpu::capture {

using namespace renderer;

namespace {

// A capture that outgrows this is dropped rather than exhausting memory
class Writer {
  public:
    template <typename T>
    void put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }
    void putBytes(const std::vector<u8>& bytes) {
        put<u64>(bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    void putString(const std::string& text) {
        put<u64>(text.size());
        out.insert(out.end(), text.begin(), text.end());
    }
    template <typename T>
    void putRaw(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        put<u64>(sizeof(T));
        put(value);
    }
    std::vector<u8> out;
};

class Reader {
  public:
    explicit Reader(const std::vector<u8>& bytes_) : bytes{bytes_} {}
    template <typename T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        need(sizeof(T));
        T value;
        std::memcpy(&value, bytes.data() + position, sizeof(T));
        position += sizeof(T);
        return value;
    }
    std::vector<u8> getBytes() {
        const auto size = get<u64>();
        need(size);
        std::vector<u8> result(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                               bytes.begin() + static_cast<std::ptrdiff_t>(position + size));
        position += size;
        return result;
    }
    std::string getString() {
        const auto data = getBytes();
        return {data.begin(), data.end()};
    }
    // A count that cannot exceed what is left (one byte per element at least)
    u64 count() {
        const auto n = get<u64>();
        if (n > bytes.size() - position)
            throw std::runtime_error("Capture holds an impossible element count");
        return n;
    }
    template <typename T>
    std::vector<u8> getRaw() {
        const auto size = get<u64>();
        if (size != sizeof(T))
            throw std::runtime_error(
                "Capture was written by a build with different state layouts");
        need(size);
        std::vector<u8> result(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                               bytes.begin() + static_cast<std::ptrdiff_t>(position + size));
        position += size;
        return result;
    }
    bool atEnd() const { return position == bytes.size(); }

  private:
    void need(u64 size) const {
        if (size > bytes.size() - position)
            throw std::runtime_error("Capture is truncated");
    }
    const std::vector<u8>& bytes;
    u64 position{0};
};

template <typename T>
std::vector<u8> raw(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<u8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
T fromRaw(const std::vector<u8>& bytes) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes.size() != sizeof(T))
        throw std::runtime_error("Capture holds a state of the wrong size");
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

void writeContents(Writer& w, const Contents& contents) {
    w.put<u64>(contents.size());
    for (const auto& blob : contents)
        w.putBytes(blob);
}

Contents readContents(Reader& r) {
    Contents contents(r.count());
    for (auto& blob : contents)
        blob = r.getBytes();
    return contents;
}

void writeBufferRef(Writer& w, const BufferRef& ref) {
    w.put(ref.buffer);
    w.put(ref.offset);
    w.put(ref.size);
}

BufferRef readBufferRef(Reader& r) {
    BufferRef ref;
    ref.buffer = r.get<i32>();
    ref.offset = r.get<u64>();
    ref.size = r.get<u64>();
    return ref;
}

void writeBufferRefs(Writer& w,
                     const std::vector<std::pair<u32, BufferRef>>& refs) {
    w.put<u64>(refs.size());
    for (const auto& [index, ref] : refs) {
        w.put(index);
        writeBufferRef(w, ref);
    }
}

std::vector<std::pair<u32, BufferRef>> readBufferRefs(Reader& r) {
    std::vector<std::pair<u32, BufferRef>> refs(r.count());
    for (auto& [index, ref] : refs) {
        index = r.get<u32>();
        ref = readBufferRef(r);
    }
    return refs;
}

template <typename Map>
void writeMap(Writer& w, const Map& map) {
    w.put<u64>(map.size());
    for (const auto& [key, value] : map) {
        w.put(key);
        w.put(value);
    }
}

void writeShader(Writer& w, const ShaderDescriptor& d) {
    w.put(d.type);
    w.put(d.backend);
    w.putBytes(d.code);
    w.put(d.writes_layer);
    const auto& m = d.resource_mapping;
    w.put(m.code_read_size);
    w.put(m.fragment_output_mask);
    w.put<u64>(m.branch_constants.size());
    for (const auto& [key, value] : m.branch_constants) {
        w.put(key.first);
        w.put(key.second);
        w.put(value);
    }
    for (const auto value : m.uniform_buffers)
        w.put(value);
    for (const auto value : m.uniform_buffer_sizes)
        w.put(value);
    w.put<u64>(m.storage_buffers.size());
    for (const auto& storage : m.storage_buffers) {
        w.put(storage.cbuf_slot);
        w.put(storage.cbuf_offset);
        w.put(storage.host_index);
        w.put(storage.written);
    }
    writeMap(w, m.textures);
    writeMap(w, m.texture_types);
    w.put<u64>(m.guessed_texture_types.size());
    for (const auto value : m.guessed_texture_types)
        w.put(value);
    w.put(m.sampler_buffer_index);
    w.put(m.stage_input_mask);
    writeMap(w, m.images);
    writeMap(w, m.image_types);
    w.put(m.writes_layer);
    w.put<u64>(m.unsupported.size());
    for (const auto& text : m.unsupported)
        w.putString(text);
}

ShaderDescriptor readShader(Reader& r) {
    ShaderDescriptor d;
    d.type = r.get<ShaderType>();
    d.backend = r.get<ShaderBackend>();
    d.code = r.getBytes();
    d.writes_layer = r.get<bool>();
    auto& m = d.resource_mapping;
    m.code_read_size = r.get<u64>();
    m.fragment_output_mask = r.get<u32>();
    for (auto n = r.count(); n > 0; n--) {
        const auto first = r.get<u32>();
        const auto second = r.get<u32>();
        m.branch_constants[{first, second}] = r.get<u32>();
    }
    for (auto& value : m.uniform_buffers)
        value = r.get<u32>();
    for (auto& value : m.uniform_buffer_sizes)
        value = r.get<u64>();
    m.storage_buffers.resize(r.count());
    for (auto& storage : m.storage_buffers) {
        storage.cbuf_slot = r.get<u32>();
        storage.cbuf_offset = r.get<u32>();
        storage.host_index = r.get<u32>();
        storage.written = r.get<bool>();
    }
    for (auto n = r.count(); n > 0; n--) {
        const auto key = r.get<u32>();
        m.textures[key] = r.get<u32>();
    }
    for (auto n = r.count(); n > 0; n--) {
        const auto key = r.get<u32>();
        m.texture_types[key] = r.get<MTL::TextureType>();
    }
    for (auto n = r.count(); n > 0; n--)
        m.guessed_texture_types.insert(r.get<u32>());
    m.sampler_buffer_index = r.get<u32>();
    m.stage_input_mask = r.get<u32>();
    for (auto n = r.count(); n > 0; n--) {
        const auto key = r.get<u32>();
        m.images[key] = r.get<u32>();
    }
    for (auto n = r.count(); n > 0; n--) {
        const auto key = r.get<u32>();
        m.image_types[key] = r.get<MTL::TextureType>();
    }
    m.writes_layer = r.get<bool>();
    m.unsupported.resize(r.count());
    for (auto& text : m.unsupported)
        text = r.getString();
    return d;
}

void writeTarget(Writer& w, const TargetRecord& t) {
    w.put(t.view);
    w.put(t.load_action_clear);
    w.put(t.clear_data);
}

TargetRecord readTarget(Reader& r) {
    TargetRecord t;
    t.view = r.get<i32>();
    t.load_action_clear = r.get<bool>();
    t.clear_data = r.get<std::array<u8, 16>>();
    return t;
}

u64 hashBytes(const u8* data, u64 size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < size; i++)
        hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

} // namespace

std::vector<u8> CaptureFile::serialize() const {
    Writer w;
    for (const char c : MAGIC)
        w.put(c);
    w.put(VERSION);

    w.put<u64>(textures.size());
    for (const auto& t : textures) {
        w.put(t.type);
        w.put(t.format);
        w.put(t.width);
        w.put(t.height);
        w.put(t.depth);
        w.put(t.levels);
        w.put(t.layers);
        w.put(t.is_linear);
        w.put(t.linear_stride);
        w.put(t.block_width_gobs_log2);
        w.put(t.block_height_gobs_log2);
        w.put(t.block_depth_gobs_log2);
        w.put(t.layer_size);
        w.putBytes(t.guest);
        writeContents(w, t.initial);
        writeContents(w, t.final_contents);
    }
    w.put<u64>(views.size());
    for (const auto& v : views) {
        w.put(v.texture);
        w.put(v.type);
        w.put(v.format);
        w.put(v.level_begin);
        w.put(v.level_end);
        w.put(v.layer_begin);
        w.put(v.layer_end);
        w.put(v.swizzle);
    }
    w.put<u64>(samplers.size());
    for (const auto& s : samplers)
        w.putBytes(s.raw);
    w.put<u64>(buffers.size());
    for (const auto& b : buffers)
        w.putBytes(b.bytes);
    w.put<u64>(shaders.size());
    for (const auto& s : shaders)
        writeShader(w, s.descriptor);
    w.put<u64>(pipelines.size());
    for (const auto& p : pipelines) {
        for (const auto id : p.shaders)
            w.put(id);
        w.putBytes(p.vertex_state);
        w.putBytes(p.color_targets);
        w.put(p.depth_stencil_format);
        w.put(p.topology_class);
    }
    w.put<u64>(passes.size());
    for (const auto& p : passes) {
        for (const auto& target : p.color)
            writeTarget(w, target);
        writeTarget(w, p.depth_stencil);
    }
    w.put<u64>(notes.size());
    for (const auto& note : notes)
        w.putString(note);
    w.put<u64>(commands.size());
    for (const auto& c : commands) {
        w.put(c.kind);
        w.put(c.origin.channel);
        w.put(c.origin.sequence);
        w.put(c.origin.source_address);
        if (c.kind == CommandRecord::Kind::Draw) {
            const auto& d = c.draw;
            w.put(d.pipeline);
            w.put(d.pass);
            w.putBytes(d.state);
            w.put(d.occlusion);
            for (const auto& ref : d.vertices)
                writeBufferRef(w, ref);
            for (const auto& stage : d.stages) {
                w.put(stage.active);
                w.put(stage.reset_textures);
                writeBufferRefs(w, stage.uniforms);
                writeBufferRefs(w, stage.storage);
                w.put<u64>(stage.textures.size());
                for (const auto& texture : stage.textures) {
                    w.put(texture.index);
                    w.put(texture.view);
                    w.put(texture.sampler);
                }
            }
            w.put(d.primitive);
            w.put(d.index_type);
            writeBufferRef(w, d.indices);
            w.put(d.first);
            w.put(d.count);
            w.put(d.base_vertex);
            w.put(d.base_instance);
            w.put(d.instance_count);
        } else if (c.kind == CommandRecord::Kind::Clear) {
            const auto& d = c.clear;
            w.put(d.pass);
            w.putBytes(d.state);
            w.put(d.has_color);
            w.put(d.has_depth);
            w.put(d.has_stencil);
            w.put(d.color_target);
            w.put(d.color_layer);
            w.put(d.color_mask);
            w.put(d.color_value);
            w.put(d.depth_layer);
            w.put(d.depth_value);
            w.put(d.stencil_layer);
            w.put(d.stencil_value);
        } else if (c.kind == CommandRecord::Kind::Copy) {
            const auto& d = c.copy;
            w.put(d.src);
            w.put(d.dst);
            w.put(d.src_origin);
            w.put(d.dst_origin);
            w.put(d.src_size);
            w.put(d.dst_size);
            w.put(d.src_level);
            w.put(d.src_layer);
            w.put(d.dst_level);
            w.put(d.dst_layer);
            w.put(d.level_count);
            w.put(d.layer_count);
            w.putBytes(d.options);
        } else if (c.kind == CommandRecord::Kind::Compute) {
            const auto& d = c.compute;
            w.put(d.shader);
            w.put(d.grid);
            w.put(d.block);
            writeBufferRefs(w, d.uniforms);
            w.put<u64>(d.storage.size());
            for (const auto& binding : d.storage) {
                w.put(binding.index);
                writeBufferRef(w, binding.buffer);
                w.put(binding.written);
                w.putBytes(binding.expected);
            }
            w.put<u64>(d.textures.size());
            for (const auto& binding : d.textures) {
                w.put(binding.index);
                w.put(binding.view);
                w.put(binding.sampler);
            }
            w.put<u64>(d.images.size());
            for (const auto& binding : d.images) {
                w.put(binding.index);
                w.put(binding.view);
            }
        } else {
            throw std::runtime_error(
                "Cannot serialize an unknown capture command");
        }
    }
    return std::move(w.out);
}

CaptureFile CaptureFile::deserialize(const std::vector<u8>& bytes) {
    Reader r(bytes);
    for (const char expected : MAGIC)
        if (r.get<char>() != expected)
            throw std::runtime_error("Not a Hydra GPU capture");
    const auto version = r.get<u32>();
    if (version != 3 && version != VERSION)
        throw std::runtime_error("Unsupported capture version");

    CaptureFile f;
    f.textures.resize(r.count());
    for (auto& t : f.textures) {
        t.type = r.get<u32>();
        t.format = r.get<u32>();
        t.width = r.get<u32>();
        t.height = r.get<u32>();
        t.depth = r.get<u32>();
        t.levels = r.get<u32>();
        t.layers = r.get<u32>();
        t.is_linear = r.get<bool>();
        t.linear_stride = r.get<u32>();
        t.block_width_gobs_log2 = r.get<u32>();
        t.block_height_gobs_log2 = r.get<u32>();
        t.block_depth_gobs_log2 = r.get<u32>();
        t.layer_size = r.get<u32>();
        t.guest = r.getBytes();
        t.initial = readContents(r);
        t.final_contents = readContents(r);
    }
    f.views.resize(r.count());
    for (auto& v : f.views) {
        v.texture = r.get<i32>();
        v.type = r.get<u32>();
        v.format = r.get<u32>();
        v.level_begin = r.get<u32>();
        v.level_end = r.get<u32>();
        v.layer_begin = r.get<u32>();
        v.layer_end = r.get<u32>();
        v.swizzle = r.get<std::array<u8, 4>>();
    }
    f.samplers.resize(r.count());
    for (auto& s : f.samplers)
        s.raw = r.getBytes();
    f.buffers.resize(r.count());
    for (auto& b : f.buffers)
        b.bytes = r.getBytes();
    f.shaders.resize(r.count());
    for (auto& s : f.shaders)
        s.descriptor = readShader(r);
    f.pipelines.resize(r.count());
    for (auto& p : f.pipelines) {
        for (auto& id : p.shaders)
            id = r.get<i32>();
        p.vertex_state = r.getBytes();
        p.color_targets = r.getBytes();
        p.depth_stencil_format = r.get<u32>();
        p.topology_class = r.get<u32>();
    }
    f.passes.resize(r.count());
    for (auto& p : f.passes) {
        for (auto& target : p.color)
            target = readTarget(r);
        p.depth_stencil = readTarget(r);
    }
    f.notes.resize(r.count());
    for (auto& note : f.notes)
        note = r.getString();
    f.commands.resize(r.count());
    for (auto& c : f.commands) {
        c.kind = r.get<CommandRecord::Kind>();
        if (version >= 4) {
            c.origin.channel = r.get<u64>();
            c.origin.sequence = r.get<u64>();
            c.origin.source_address = r.get<u64>();
        }
        if (c.kind == CommandRecord::Kind::Draw) {
            auto& d = c.draw;
            d.pipeline = r.get<i32>();
            d.pass = r.get<i32>();
            d.state = r.getBytes();
            d.occlusion = r.get<bool>();
            for (auto& ref : d.vertices)
                ref = readBufferRef(r);
            for (auto& stage : d.stages) {
                stage.active = r.get<bool>();
                stage.reset_textures = r.get<bool>();
                stage.uniforms = readBufferRefs(r);
                stage.storage = readBufferRefs(r);
                stage.textures.resize(r.count());
                for (auto& texture : stage.textures) {
                    texture.index = r.get<u32>();
                    texture.view = r.get<i32>();
                    texture.sampler = r.get<i32>();
                }
            }
            d.primitive = r.get<u32>();
            d.index_type = r.get<u32>();
            d.indices = readBufferRef(r);
            d.first = r.get<u32>();
            d.count = r.get<u32>();
            d.base_vertex = r.get<u32>();
            d.base_instance = r.get<u32>();
            d.instance_count = r.get<u32>();
        } else if (c.kind == CommandRecord::Kind::Clear) {
            auto& d = c.clear;
            d.pass = r.get<i32>();
            d.state = r.getBytes();
            d.has_color = r.get<bool>();
            d.has_depth = r.get<bool>();
            d.has_stencil = r.get<bool>();
            d.color_target = r.get<u32>();
            d.color_layer = r.get<u32>();
            d.color_mask = r.get<u32>();
            d.color_value = r.get<std::array<u32, 4>>();
            d.depth_layer = r.get<u32>();
            d.depth_value = r.get<f32>();
            d.stencil_layer = r.get<u32>();
            d.stencil_value = r.get<u32>();
        } else if (version >= 4 && c.kind == CommandRecord::Kind::Copy) {
            auto& d = c.copy;
            d.src = r.get<i32>();
            d.dst = r.get<i32>();
            d.src_origin = r.get<std::array<f32, 3>>();
            d.dst_origin = r.get<std::array<f32, 3>>();
            d.src_size = r.get<std::array<u64, 3>>();
            d.dst_size = r.get<std::array<u64, 3>>();
            d.src_level = r.get<u32>();
            d.src_layer = r.get<u32>();
            d.dst_level = r.get<u32>();
            d.dst_layer = r.get<u32>();
            d.level_count = r.get<u32>();
            d.layer_count = r.get<u32>();
            d.options = r.getRaw<BlitOptions>();
        } else if (version >= 4 && c.kind == CommandRecord::Kind::Compute) {
            auto& d = c.compute;
            d.shader = r.get<i32>();
            d.grid = r.get<std::array<u64, 3>>();
            d.block = r.get<std::array<u64, 3>>();
            d.uniforms = readBufferRefs(r);
            d.storage.resize(r.count());
            for (auto& binding : d.storage) {
                binding.index = r.get<u32>();
                binding.buffer = readBufferRef(r);
                binding.written = r.get<bool>();
                binding.expected = r.getBytes();
            }
            d.textures.resize(r.count());
            for (auto& binding : d.textures) {
                binding.index = r.get<u32>();
                binding.view = r.get<i32>();
                binding.sampler = r.get<i32>();
            }
            d.images.resize(r.count());
            for (auto& binding : d.images) {
                binding.index = r.get<u32>();
                binding.view = r.get<i32>();
            }
        } else {
            throw std::runtime_error("Capture holds an unknown command");
        }
    }
    if (!r.atEnd())
        throw std::runtime_error("Capture has trailing data");
    return f;
}

void CaptureFile::save(const std::filesystem::path& path) const {
    const auto bytes = serialize();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());
}

CaptureFile CaptureFile::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read " + path.string());
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    return deserialize(bytes);
}

// ------------------------------------------------------------------ Recorder

i32 Recorder::texture(ICommandBuffer* command_buffer, ITexture* object) {
    if (const auto it = textures.find(object); it != textures.end())
        return it->second;
    const auto& d = object->getDescriptor();
    TextureRecord record{static_cast<u32>(d.type), static_cast<u32>(d.format),
                         d.width,  d.height, d.depth,
                         d.level_count, d.layer_count, d.is_linear,
                         d.linear_stride, d.block_width_gobs_log2,
                         d.block_height_gobs_log2, d.block_depth_gobs_log2,
                         d.layer_size, {}, {}, {}};
    // The guest bytes behind it (a host pointer into guest memory)
    constexpr u64 MAX_GUEST_BYTES = 8u << 20;
    if (d.ptr != 0 && d.type != MTL::TextureTypeTextureBuffer &&
        d.size != 0 && d.size <= MAX_GUEST_BYTES &&
        capture_bytes + d.size < limit_bytes) {
        const auto* guest = reinterpret_cast<const u8*>(d.ptr);
        record.guest.assign(guest, guest + d.size);
        capture_bytes += d.size;
    }
    // Earlier GPU work may still be writing it
    renderer.synchronize(command_buffer);
    if (!renderer.readTextureContents(object, record.initial)) {
        // Replay without the contents rather than lose the capture
        record.initial.clear();
        file.notes.push_back(fmt::format(
            "texture {} ({}, {}x{}x{}, {} levels, {} layers) has no recorded "
            "contents",
            d.format, d.type, d.width, d.height, d.depth, d.level_count,
            d.layer_count));
    }
    u64 bytes = 0;
    for (const auto& blob : record.initial)
        bytes += blob.size();
    capture_bytes += bytes;
    const auto id = static_cast<i32>(file.textures.size());
    file.textures.push_back(std::move(record));
    texture_objects.push_back(object);
    written.push_back(false);
    textures[object] = id;
    return id;
}

i32 Recorder::view(ICommandBuffer* command_buffer, ITextureView* object) {
    if (!object)
        return -1;
    if (const auto it = views.find(object); it != views.end())
        return it->second;
    const auto base = texture(command_buffer, object->getBase());
    if (base < 0)
        return -1;
    const auto& d = object->getDescriptor();
    ViewRecord record{base,
                      static_cast<u32>(d.type),
                      static_cast<u32>(d.format),
                      d.levels.getBegin(),
                      d.levels.getEnd(),
                      d.layers.getBegin(),
                      d.layers.getEnd(),
                      {static_cast<u8>(d.swizzle_channels.r),
                       static_cast<u8>(d.swizzle_channels.g),
                       static_cast<u8>(d.swizzle_channels.b),
                       static_cast<u8>(d.swizzle_channels.a)}};
    const auto id = static_cast<i32>(file.views.size());
    file.views.push_back(record);
    views[object] = id;
    return id;
}

i32 Recorder::sampler(SamplerBase* object) {
    if (!object)
        return -1;
    if (const auto it = samplers.find(object); it != samplers.end())
        return it->second;
    const auto id = static_cast<i32>(file.samplers.size());
    file.samplers.push_back({raw(object->getDescriptor())});
    samplers[object] = id;
    return id;
}

i32 Recorder::shader(ShaderBase* object) {
    if (!object)
        return -1;
    if (const auto it = shaders.find(object); it != shaders.end())
        return it->second;
    const auto id = static_cast<i32>(file.shaders.size());
    file.shaders.push_back({object->getDescriptor()});
    shaders[object] = id;
    return id;
}

i32 Recorder::pipeline(ICommandBuffer*, PipelineBase* object) {
    if (!object)
        return -1;
    if (const auto it = pipelines.find(object); it != pipelines.end())
        return it->second;
    const auto& d = object->getDescriptor();
    PipelineRecord record{};
    for (usize i = 0; i < static_cast<usize>(ShaderType::Count); i++)
        record.shaders[i] = shader(d.shaders[i]);
    record.vertex_state = raw(d.vertex_state);
    std::vector<u8> targets;
    for (const auto& target : d.color_target_states) {
        const auto bytes = raw(target);
        targets.insert(targets.end(), bytes.begin(), bytes.end());
    }
    record.color_targets = std::move(targets);
    record.depth_stencil_format = static_cast<u32>(d.depth_stencil_format);
    record.topology_class = static_cast<u32>(d.topology_class);
    const auto id = static_cast<i32>(file.pipelines.size());
    file.pipelines.push_back(std::move(record));
    pipelines[object] = id;
    return id;
}

i32 Recorder::pass(ICommandBuffer* command_buffer, RenderPassBase* object,
                   bool will_write) {
    if (!object)
        return -1;
    auto it = passes.find(object);
    if (it == passes.end()) {
        const auto& d = object->getDescriptor();
        PassRecord record;
        const auto convert = [&](const RenderTargetDescriptor& source,
                                 TargetRecord& target) {
            target.view = view(command_buffer, source.texture);
            target.load_action_clear = source.load_action_clear;
            static_assert(sizeof(source.clear_data) == 16);
            std::memcpy(target.clear_data.data(), &source.clear_data, 16);
        };
        for (usize i = 0; i < COLOR_TARGET_COUNT; i++)
            convert(d.color_targets[i], record.color[i]);
        convert(d.depth_stencil_target, record.depth_stencil);
        const auto id = static_cast<i32>(file.passes.size());
        file.passes.push_back(record);
        it = passes.emplace(object, id).first;
    }
    if (will_write) {
        const auto& record = file.passes[static_cast<usize>(it->second)];
        const auto mark = [&](const TargetRecord& target) {
            if (target.view >= 0)
                written[static_cast<usize>(
                    file.views[static_cast<usize>(target.view)].texture)] = true;
        };
        for (const auto& target : record.color)
            mark(target);
        mark(record.depth_stencil);
    }
    return it->second;
}

BufferRef Recorder::buffer(const BufferView& view) {
    if (!view.isValid() || view.getSize() == 0)
        return {};
    const auto size = view.getSize();
    std::vector<u8> bytes;
    if (!renderer.readBufferContents(view, bytes)) {
        fail("A buffer cannot be read back");
        return {};
    }
    const auto key = std::make_tuple(static_cast<const void*>(view.getBase()),
                                     view.getOffset(), size,
                                     hashBytes(bytes.data(), size));
    auto it = buffers.find(key);
    if (it == buffers.end()) {
        const auto id = static_cast<i32>(file.buffers.size());
        file.buffers.push_back({std::move(bytes)});
        capture_bytes += size;
        it = buffers.emplace(key, id).first;
    }
    return {it->second, 0, size};
}

bool Recorder::recordDraw(ICommandBuffer* command_buffer,
                          const command_ir::DrawCommand& command) {
    commandExecuted(command_buffer);
    if (!error.empty())
        return false;
    if (capture_bytes >= limit_bytes) {
        reached_limit = true;
        return false;
    }
    if (!command.pipeline || !command.render_pass)
        return fail("Draw without a pipeline or render pass");
    // Earlier work may still be writing the buffers it reads
    if (!renderer.synchronize(command_buffer))
        return fail("Earlier GPU work failed before draw capture");
    CommandRecord record;
    record.kind = CommandRecord::Kind::Draw;
    record.origin = command.origin;
    auto& d = record.draw;
    d.pipeline = pipeline(command_buffer, command.pipeline);
    d.pass = pass(command_buffer, command.render_pass, true);
    d.state = raw(command.state);
    d.occlusion = command.occlusion_enabled;
    for (usize i = 0; i < VERTEX_ARRAY_COUNT; i++)
        d.vertices[i] = buffer(command.vertices[i]);
    for (usize i = 0; i < GRAPHICS_SHADER_TYPE_COUNT; i++) {
        const auto& source = command.stages[i];
        auto& stage = d.stages[i];
        stage.active = source.active;
        stage.reset_textures = source.reset_textures;
        for (const auto& binding : source.uniforms)
            stage.uniforms.emplace_back(binding.index, buffer(binding.buffer));
        for (const auto& binding : source.storage)
            stage.storage.emplace_back(binding.index, buffer(binding.buffer));
        for (const auto& binding : source.textures)
            stage.textures.push_back({binding.index,
                                      view(command_buffer, binding.texture),
                                      sampler(binding.sampler)});
    }
    d.primitive = static_cast<u32>(command.primitive);
    d.index_type = static_cast<u32>(command.index_type);
    d.indices = buffer(command.indices);
    d.first = command.first;
    d.count = command.count;
    d.base_vertex = command.base_vertex;
    d.base_instance = command.base_instance;
    d.instance_count = command.instance_count;
    if (!error.empty())
        return false;
    file.commands.push_back(std::move(record));
    return true;
}

bool Recorder::recordClear(ICommandBuffer* command_buffer,
                           const command_ir::ClearCommand& command) {
    commandExecuted(command_buffer);
    if (!error.empty())
        return false;
    if (!command.render_pass)
        return true;
    if (capture_bytes >= limit_bytes) {
        reached_limit = true;
        return false;
    }
    CommandRecord record;
    record.kind = CommandRecord::Kind::Clear;
    record.origin = command.resources.origin;
    auto& d = record.clear;
    d.pass = pass(command_buffer, command.render_pass, true);
    d.state = raw(command.state);
    if (command.color) {
        d.has_color = true;
        d.color_target = command.color->target;
        d.color_layer = command.color->layer;
        d.color_mask = command.color->mask;
        d.color_value = {command.color->value[0], command.color->value[1],
                         command.color->value[2], command.color->value[3]};
    }
    if (command.depth) {
        d.has_depth = true;
        d.depth_layer = command.depth->layer;
        d.depth_value = command.depth->value;
    }
    if (command.stencil) {
        d.has_stencil = true;
        d.stencil_layer = command.stencil->layer;
        d.stencil_value = command.stencil->value;
    }
    if (!error.empty())
        return false;
    file.commands.push_back(std::move(record));
    return true;
}

bool Recorder::recordCopy(ICommandBuffer* commands,
                          const command_ir::CopyCommand& command) {
    commandExecuted(commands);
    if (!error.empty())
        return false;
    if (capture_bytes >= limit_bytes) {
        reached_limit = true;
        return false;
    }
    if (!command.src || !command.dst || !command.resources.valid(renderer))
        return fail("Copy capture requires sealed live resources");
    if (!renderer.synchronize(commands))
        return fail("Earlier GPU work failed before copy capture");
    CommandRecord record;
    record.kind = CommandRecord::Kind::Copy;
    record.origin = command.resources.origin;
    auto& d = record.copy;
    d.src = view(commands, command.src);
    d.dst = view(commands, command.dst);
    d.src_origin = {command.src_origin[0], command.src_origin[1],
                    command.src_origin[2]};
    d.dst_origin = {command.dst_origin[0], command.dst_origin[1],
                    command.dst_origin[2]};
    d.src_size = {command.src_size.width, command.src_size.height,
                  command.src_size.depth};
    d.dst_size = {command.dst_size.width, command.dst_size.height,
                  command.dst_size.depth};
    d.src_level = command.src_level;
    d.src_layer = command.src_layer;
    d.dst_level = command.dst_level;
    d.dst_layer = command.dst_layer;
    d.level_count = command.level_count;
    d.layer_count = command.layer_count;
    d.options = raw(command.options);
    if (!error.empty() || d.src < 0 || d.dst < 0)
        return fail("A copied texture could not be captured");
    written[static_cast<usize>(file.views[static_cast<usize>(d.dst)].texture)] =
        true;
    file.commands.push_back(std::move(record));
    return true;
}

bool Recorder::recordCompute(ICommandBuffer* commands,
                             const command_ir::ComputeCommand& command) {
    commandExecuted(commands);
    if (!error.empty())
        return false;
    const auto& dispatch = command.dispatch;
    if (command_ir::emptyComputeGrid(dispatch.grid_size.width,
                                     dispatch.grid_size.height,
                                     dispatch.grid_size.depth) ||
        command_ir::emptyComputeGrid(dispatch.block_size.width,
                                     dispatch.block_size.height,
                                     dispatch.block_size.depth))
        return true;
    if (capture_bytes >= limit_bytes) {
        reached_limit = true;
        return false;
    }
    if (!command_ir::validateCompute(renderer, command))
        return fail("Compute capture requires sealed live resources");
    if (!renderer.synchronize(commands))
        return fail("Earlier GPU work failed before compute capture");
    CommandRecord record;
    record.kind = CommandRecord::Kind::Compute;
    record.origin = command.resources.origin;
    auto& d = record.compute;
    d.shader = shader(dispatch.shader);
    d.grid = {dispatch.grid_size.width, dispatch.grid_size.height,
              dispatch.grid_size.depth};
    d.block = {dispatch.block_size.width, dispatch.block_size.height,
               dispatch.block_size.depth};
    // Read whole native buffers so overlapping bindings of the same buffer
    // retain one backing allocation and their original offsets in replay.
    std::unordered_map<BufferBase*, BufferRef> captured_buffers;
    const auto capture_buffer = [&](const BufferView& source) -> BufferRef {
        if (!source.isValid())
            return {};
        if (const auto found = captured_buffers.find(source.getBase());
            found != captured_buffers.end())
            return {found->second.buffer, source.getOffset(), source.getSize()};
        const auto ptr = source.getBase()->getPtr();
        for (const auto& [base, ref] : captured_buffers) {
            (void)ref;
            const auto other = base->getPtr();
            if (ptr != 0 && other != 0 &&
                (ptr <= other ? other - ptr < source.getBase()->getSize()
                              : ptr - other < base->getSize())) {
                fail("Compute capture cannot restore aliases spanning "
                     "different native buffers");
                return {};
            }
        }
        if (source.getBase()->getSize() >
            limit_bytes - std::min(limit_bytes, capture_bytes)) {
            fail("A compute buffer exceeds the remaining capture budget");
            return {};
        }
        auto ref = buffer(BufferView(source.getBase()));
        captured_buffers.emplace(source.getBase(), ref);
        ref.offset = source.getOffset();
        ref.size = source.getSize();
        return ref;
    };
    for (u32 i = 0; i < dispatch.uniform_buffers.size(); ++i)
        if (dispatch.uniform_buffers[i].isValid())
            d.uniforms.emplace_back(
                i, capture_buffer(dispatch.uniform_buffers[i]));
    for (const auto& binding : dispatch.storage_buffers) {
        d.storage.push_back({binding.index,
                             capture_buffer(binding.buffer),
                             binding.written,
                             {}});
        if (binding.written)
            pending_compute_buffers.emplace_back(d.storage.size() - 1,
                                                 binding.buffer);
    }
    for (const auto& binding : dispatch.textures)
        d.textures.push_back({binding.index, view(commands, binding.texture),
                              sampler(binding.sampler)});
    for (const auto& binding : dispatch.images) {
        const auto id = view(commands, binding.texture);
        d.images.push_back({binding.index, id});
        if (id >= 0)
            written[static_cast<usize>(
                file.views[static_cast<usize>(id)].texture)] = true;
    }
    if (!error.empty()) {
        pending_compute_buffers.clear();
        return false;
    }
    file.commands.push_back(std::move(record));
    return true;
}

void Recorder::commandExecuted(ICommandBuffer* commands) {
    if (pending_compute_buffers.empty() || !error.empty())
        return;
    if (!renderer.synchronize(commands)) {
        fail("GPU work failed before compute results could be captured");
        pending_compute_buffers.clear();
        return;
    }
    auto& compute = file.commands.back().compute;
    for (const auto& [index, source] : pending_compute_buffers) {
        auto& expected = compute.storage[index].expected;
        if (source.getSize() >
                limit_bytes - std::min(limit_bytes, capture_bytes) ||
            !renderer.readBufferContents(source, expected) ||
            expected.size() != source.getSize()) {
            fail("A compute storage result cannot be captured within the "
                 "budget");
            break;
        }
        capture_bytes += expected.size();
    }
    pending_compute_buffers.clear();
}

CaptureFile Recorder::finish(ICommandBuffer* command_buffer) {
    commandExecuted(command_buffer);
    if (!renderer.synchronize(command_buffer))
        fail("GPU work failed while finishing the capture");
    for (usize i = 0; i < texture_objects.size(); i++) {
        if (!written[i])
            continue;
        if (!renderer.readTextureContents(texture_objects[i],
                                          file.textures[i].final_contents)) {
            file.textures[i].final_contents.clear();
            file.notes.push_back(fmt::format(
                "written texture {} has no recorded result", i));
        }
    }
    return std::move(file);
}

// -------------------------------------------------------------------- Replay

ReplayResult replay(IRenderer& renderer, const CaptureFile& file,
                    usize command_limit, usize step,
                    const ReplayStepCallback& on_step, usize first_step) {
    ReplayResult result;
    const auto at = [&](auto& vector, i32 id) -> auto& {
        if (id < 0 || static_cast<usize>(id) >= vector.size())
            throw std::runtime_error("Capture refers to a missing object");
        return vector[static_cast<usize>(id)];
    };
    try {
        std::vector<std::unique_ptr<ITexture>> textures;
        std::vector<std::unique_ptr<ITextureView>> views;
        std::vector<std::unique_ptr<SamplerBase>> samplers;
        std::vector<std::unique_ptr<BufferBase>> buffers;
        std::vector<std::unique_ptr<ShaderBase>> shaders;
        std::vector<std::unique_ptr<PipelineBase>> pipelines;
        std::vector<std::unique_ptr<RenderPassBase>> passes;
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());

        for (const auto& t : file.textures) {
            textures.emplace_back(renderer.createTexture(TextureDescriptor(
                0, static_cast<MTL::TextureType>(t.type),
                static_cast<TextureFormat>(t.format), t.is_linear,
                t.linear_stride, t.width, t.height, t.depth, t.levels, t.layers,
                t.block_width_gobs_log2, t.block_height_gobs_log2,
                t.block_depth_gobs_log2, t.layer_size)));
            if (!t.initial.empty() &&
                !renderer.writeTextureContents(textures.back().get(), t.initial))
                throw std::runtime_error("A texture cannot be restored");
        }
        for (const auto& v : file.views) {
            auto& base = at(textures, v.texture);
            views.emplace_back(base->createView(TextureViewDescriptor(
                static_cast<MTL::TextureType>(v.type),
                static_cast<TextureFormat>(v.format),
                ztd::Range<u32>(v.level_begin, v.level_end),
                ztd::Range<u32>(v.layer_begin, v.layer_end),
                SwizzleChannels(static_cast<ImageSwizzle>(v.swizzle[0]),
                                static_cast<ImageSwizzle>(v.swizzle[1]),
                                static_cast<ImageSwizzle>(v.swizzle[2]),
                                static_cast<ImageSwizzle>(v.swizzle[3])))));
        }
        for (const auto& s : file.samplers)
            samplers.emplace_back(
                renderer.createSampler(fromRaw<SamplerDescriptor>(s.raw)));
        for (const auto& b : file.buffers) {
            // CPU-mappable, as the immutable uploads of the engines
            buffers.emplace_back(renderer.allocateTemporaryBuffer(
                std::max<usize>(b.bytes.size(), 1)));
            if (!b.bytes.empty())
                buffers.back()->copyFrom(reinterpret_cast<uptr>(b.bytes.data()));
        }
        for (const auto& s : file.shaders)
            shaders.emplace_back(renderer.createShader(s.descriptor));
        for (const auto& p : file.pipelines) {
            PipelineDescriptor d{};
            for (usize i = 0; i < static_cast<usize>(ShaderType::Count); i++)
                d.shaders[i] =
                    p.shaders[i] < 0 ? nullptr : at(shaders, p.shaders[i]).get();
            d.vertex_state = fromRaw<VertexState>(p.vertex_state);
            if (p.color_targets.size() !=
                sizeof(ColorTargetState) * COLOR_TARGET_COUNT)
                throw std::runtime_error("Capture holds wrong color targets");
            std::memcpy(d.color_target_states, p.color_targets.data(),
                        p.color_targets.size());
            d.depth_stencil_format =
                static_cast<TextureFormat>(p.depth_stencil_format);
            d.topology_class =
                static_cast<MTL::PrimitiveTopologyClass>(p.topology_class);
            pipelines.emplace_back(renderer.createPipeline(d));
        }
        for (const auto& p : file.passes) {
            RenderPassDescriptor d{};
            const auto convert = [&](const TargetRecord& source,
                                     RenderTargetDescriptor& target) {
                target.texture =
                    source.view < 0 ? nullptr : at(views, source.view).get();
                target.load_action_clear = source.load_action_clear;
                std::memcpy(&target.clear_data, source.clear_data.data(), 16);
            };
            for (usize i = 0; i < COLOR_TARGET_COUNT; i++)
                convert(p.color[i], d.color_targets[i]);
            convert(p.depth_stencil, d.depth_stencil_target);
            passes.emplace_back(renderer.createRenderPass(d));
        }

        const auto buffer_view = [&](const BufferRef& ref) {
            if (ref.buffer < 0)
                return BufferView{};
            auto* base = at(buffers, ref.buffer).get();
            if (ref.offset > base->getSize() ||
                ref.size > base->getSize() - ref.offset)
                throw std::runtime_error(
                    "Capture buffer reference is out of bounds");
            return BufferView(base, ref.offset, ref.size);
        };
        result.matches = true;
        usize executed = 0;
        const auto report = [&] {
            renderer.synchronize(commands.get());
            std::vector<Contents> written(file.textures.size());
            for (usize i = 0; i < file.textures.size(); i++)
                if (!file.textures[i].final_contents.empty())
                    renderer.readTextureContents(textures[i].get(), written[i]);
            on_step(executed, written);
        };
        for (const auto& c : file.commands) {
            if (executed >= command_limit)
                break;
            if (c.kind == CommandRecord::Kind::Draw) {
                const auto& d = c.draw;
                command_ir::DrawCommand command;
                command.pipeline = at(pipelines, d.pipeline).get();
                command.render_pass = at(passes, d.pass).get();
                command.state = fromRaw<DrawState>(d.state);
                command.occlusion_enabled = d.occlusion;
                for (usize i = 0; i < VERTEX_ARRAY_COUNT; i++)
                    command.vertices[i] = buffer_view(d.vertices[i]);
                for (usize i = 0; i < GRAPHICS_SHADER_TYPE_COUNT; i++) {
                    auto& stage = command.stages[i];
                    stage.active = d.stages[i].active;
                    stage.reset_textures = d.stages[i].reset_textures;
                    for (const auto& [index, ref] : d.stages[i].uniforms)
                        stage.uniforms.push_back({index, buffer_view(ref)});
                    for (const auto& [index, ref] : d.stages[i].storage)
                        stage.storage.push_back({index, buffer_view(ref)});
                    for (const auto& binding : d.stages[i].textures)
                        stage.textures.push_back(
                            {binding.index,
                             binding.view < 0 ? nullptr
                                              : at(views, binding.view).get(),
                             binding.sampler < 0
                                 ? nullptr
                                 : at(samplers, binding.sampler).get()});
                }
                command.primitive = static_cast<engines::PrimitiveType>(d.primitive);
                command.index_type = static_cast<engines::IndexType>(d.index_type);
                command.indices = buffer_view(d.indices);
                command.first = d.first;
                command.count = d.count;
                command.base_vertex = d.base_vertex;
                command.base_instance = d.base_instance;
                command.instance_count = d.instance_count;
                command_ir::captureDrawAddressSpace(renderer, command, nullptr);
                if (!command_ir::sealDrawResources(renderer, command) ||
                    !command_ir::executeDraw(renderer, commands.get(), command))
                    throw std::runtime_error("A captured draw was rejected");
            } else if (c.kind == CommandRecord::Kind::Clear) {
                const auto& d = c.clear;
                command_ir::ClearCommand command;
                command.resources.capture(renderer, nullptr);
                command.render_pass = at(passes, d.pass).get();
                command.state = fromRaw<ClearState>(d.state);
                if (d.has_color)
                    command.color = command_ir::ClearCommand::Color{
                        d.color_target, d.color_layer, static_cast<u8>(d.color_mask),
                        simd::uint4{d.color_value[0], d.color_value[1],
                                    d.color_value[2], d.color_value[3]}};
                if (d.has_depth)
                    command.depth = command_ir::ClearCommand::Depth{
                        d.depth_layer, d.depth_value};
                if (d.has_stencil)
                    command.stencil = command_ir::ClearCommand::Stencil{
                        d.stencil_layer, d.stencil_value};
                if (!command_ir::sealClear(renderer, command) ||
                    !command_ir::executeClear(renderer, commands.get(), command))
                    throw std::runtime_error("A captured clear was rejected");
            } else if (c.kind == CommandRecord::Kind::Copy) {
                const auto& d = c.copy;
                command_ir::CopyCommand command;
                command.src = at(views, d.src).get();
                command.dst = at(views, d.dst).get();
                command.src_origin = {d.src_origin[0], d.src_origin[1],
                                      d.src_origin[2]};
                command.dst_origin = {d.dst_origin[0], d.dst_origin[1],
                                      d.dst_origin[2]};
                command.src_size = {d.src_size[0], d.src_size[1],
                                    d.src_size[2]};
                command.dst_size = {d.dst_size[0], d.dst_size[1],
                                    d.dst_size[2]};
                command.src_level = d.src_level;
                command.src_layer = d.src_layer;
                command.dst_level = d.dst_level;
                command.dst_layer = d.dst_layer;
                command.level_count = d.level_count;
                command.layer_count = d.layer_count;
                command.options = fromRaw<BlitOptions>(d.options);
                if (!command_ir::sealCopy(renderer, command) ||
                    !command_ir::executeCopy(renderer, commands.get(), command))
                    throw std::runtime_error("A captured copy was rejected");
            } else if (c.kind == CommandRecord::Kind::Compute) {
                const auto& d = c.compute;
                command_ir::ComputeCommand command;
                auto& dispatch = command.dispatch;
                dispatch.shader = at(shaders, d.shader).get();
                dispatch.grid_size = {d.grid[0], d.grid[1], d.grid[2]};
                dispatch.block_size = {d.block[0], d.block[1], d.block[2]};
                for (const auto& [index, ref] : d.uniforms) {
                    if (index >= dispatch.uniform_buffers.size())
                        throw std::runtime_error(
                            "Capture compute uniform index is out of bounds");
                    dispatch.uniform_buffers[index] = buffer_view(ref);
                }
                for (const auto& binding : d.storage)
                    dispatch.storage_buffers.push_back(
                        {binding.index, buffer_view(binding.buffer),
                         binding.written});
                for (const auto& binding : d.textures)
                    dispatch.textures.push_back(
                        {binding.index,
                         binding.view < 0 ? nullptr
                                          : at(views, binding.view).get(),
                         binding.sampler < 0
                             ? nullptr
                             : at(samplers, binding.sampler).get()});
                for (const auto& binding : d.images)
                    dispatch.images.push_back(
                        {binding.index, at(views, binding.view).get()});
                if (!command_ir::sealCompute(renderer, command) ||
                    !command_ir::executeCompute(renderer, commands.get(),
                                                command) ||
                    !renderer.synchronize(commands.get()))
                    throw std::runtime_error(
                        "A captured compute was rejected or failed");
                for (const auto& binding : d.storage) {
                    if (!binding.written)
                        continue;
                    if (binding.expected.size() != binding.buffer.size)
                        throw std::runtime_error(
                            "Capture lacks a complete compute storage result");
                    std::vector<u8> actual;
                    if (!renderer.readBufferContents(
                            buffer_view(binding.buffer), actual))
                        throw std::runtime_error(
                            "A replayed storage buffer cannot be read");
                    if (actual != binding.expected) {
                        result.matches = false;
                        if (result.mismatch.empty())
                            result.mismatch = fmt::format(
                                "compute command {} storage {} differs",
                                executed, binding.index);
                    }
                }
            } else {
                throw std::runtime_error("Capture holds an unknown command");
            }
            ++executed;
            if (step != 0 && on_step && executed >= first_step &&
                executed % step == 0)
                report();
        }
        if (step != 0 && on_step && executed >= first_step &&
            executed % step != 0)
            report();
        if (!renderer.synchronize(commands.get()))
            throw std::runtime_error(
                "GPU replay did not complete successfully");
        result.contents.resize(file.textures.size());
        const bool partial = command_limit < file.commands.size();
        for (usize i = 0; i < file.textures.size(); i++) {
            if (file.textures[i].final_contents.empty())
                continue;
            if (!renderer.readTextureContents(textures[i].get(),
                                              result.contents[i]))
                throw std::runtime_error("A replayed texture cannot be read");
            if (!partial && result.contents[i] != file.textures[i].final_contents) {
                result.matches = false;
                if (result.mismatch.empty())
                    result.mismatch = fmt::format("texture {} differs", i);
            }
        }
        result.ok = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

// ---------------------------------------------------------------- LiveCapture

LiveCapture& LiveCapture::instance() {
    static LiveCapture capture;
    return capture;
}

LiveCapture::LiveCapture() {
    if (const char* dir = std::getenv("HYDRA_GPU_CAPTURE"))
        directory = dir;
}

void LiveCapture::onPresent(IRenderer& renderer) {
    std::filesystem::path watched;
    {
        std::scoped_lock lock(mutex);
        watched = directory;
    }
    if (watched.empty() || running.load(std::memory_order_acquire))
        return;
    const auto trigger = watched / "trigger";
    std::error_code ec;
    if (!std::filesystem::exists(trigger, ec))
        return;
    usize commands = 2;
    usize skip = 0;
    u64 limit_mb = 1024;
    {
        std::ifstream in(trigger);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (const auto pos = text.find("commands="); pos != std::string::npos)
            commands = std::max<usize>(1, std::strtoull(text.c_str() + pos + 9,
                                                        nullptr, 10));
        if (const auto pos = text.find("skip="); pos != std::string::npos)
            skip = std::strtoull(text.c_str() + pos + 5, nullptr, 10);
        if (const auto pos = text.find("limit_mb="); pos != std::string::npos)
            limit_mb = std::max<u64>(
                1, std::strtoull(text.c_str() + pos + 9, nullptr, 10));
    }
    std::filesystem::remove(trigger, ec);
    start(renderer, commands, skip, limit_mb << 20);
}

void LiveCapture::start(IRenderer& renderer, usize commands, usize skip,
                        u64 limit_bytes) {
    std::scoped_lock lock(mutex);
    if (recorder)
        return;
    recorder = std::make_unique<Recorder>(renderer);
    recorder->setLimit(limit_bytes);
    limit = commands;
    to_skip = skip;
    running.store(true, std::memory_order_release);
    LOG_INFO(Gpu, "GPU capture of {} commands started after skipping {}",
             commands, skip);
}

void LiveCapture::finishIfComplete(ICommandBuffer* command_buffer) {
    const bool failed = !recorder->failure().empty();
    if (!failed && !recorder->full() &&
        (recorder->commandCount() < limit || !last_executed))
        return;
    complete(command_buffer);
}

void LiveCapture::complete(ICommandBuffer* command_buffer) {
    if (!recorder->failure().empty()) {
        LOG_ERROR(Gpu, "GPU capture dropped: {}", recorder->failure());
    } else {
        const bool cut_short = recorder->commandCount() < limit;
        auto file = recorder->finish(command_buffer);
        if (cut_short)
            file.notes.push_back(fmt::format(
                "the capture stopped after {} of {} commands: size limit",
                file.commands.size(), limit));
        if (!recorder->failure().empty()) {
            LOG_ERROR(Gpu, "GPU capture dropped: {}", recorder->failure());
        } else {
            const auto path =
                directory / fmt::format("capture_{}.hcap", sequence++);
            try {
                file.save(path);
                LOG_INFO(Gpu, "GPU capture of {} commands written to {}",
                         file.commands.size(), path.string());
                for (const auto& note : file.notes)
                    LOG_WARN(Gpu, "GPU capture note: {}", note);
            } catch (const std::exception& error) {
                LOG_ERROR(Gpu, "GPU capture not written: {}", error.what());
            }
        }
    }
    recorder.reset();
    running.store(false, std::memory_order_release);
}

void LiveCapture::draw(IRenderer&, ICommandBuffer* command_buffer,
                       const command_ir::DrawCommand& command) {
    std::scoped_lock lock(mutex);
    if (!recorder || recorder->commandCount() >= limit)
        return;
    if (to_skip != 0) {
        --to_skip;
        return;
    }
    recorder->recordDraw(command_buffer, command);
    // At the size limit the capture ends here, before this command runs
    if (recorder->full())
        complete(command_buffer);
}

void LiveCapture::clear(IRenderer&, ICommandBuffer* command_buffer,
                        const command_ir::ClearCommand& command) {
    std::scoped_lock lock(mutex);
    if (!recorder || recorder->commandCount() >= limit)
        return;
    if (to_skip != 0) {
        --to_skip;
        return;
    }
    recorder->recordClear(command_buffer, command);
    if (recorder->full())
        complete(command_buffer);
}

void LiveCapture::copy(IRenderer&, ICommandBuffer* command_buffer,
                       const command_ir::CopyCommand& command) {
    std::scoped_lock lock(mutex);
    if (!recorder || recorder->commandCount() >= limit)
        return;
    if (to_skip != 0) {
        --to_skip;
        return;
    }
    recorder->recordCopy(command_buffer, command);
    if (recorder->full())
        complete(command_buffer);
}

void LiveCapture::compute(IRenderer&, ICommandBuffer* command_buffer,
                          const command_ir::ComputeCommand& command) {
    std::scoped_lock lock(mutex);
    if (!recorder || recorder->commandCount() >= limit)
        return;
    if (to_skip != 0) {
        --to_skip;
        return;
    }
    recorder->recordCompute(command_buffer, command);
    if (recorder->full())
        complete(command_buffer);
}

void LiveCapture::abort(std::string_view reason) {
    std::scoped_lock lock(mutex);
    if (recorder)
        LOG_ERROR(Gpu, "GPU capture dropped: {}", reason);
    recorder.reset();
    running.store(false, std::memory_order_release);
}

void LiveCapture::commandExecuted(ICommandBuffer* command_buffer) {
    std::scoped_lock lock(mutex);
    if (!recorder)
        return;
    recorder->commandExecuted(command_buffer);
    // A failed recording ends the capture at once; otherwise the capture ends
    // with the execution of its last command
    last_executed = recorder->commandCount() >= limit;
    finishIfComplete(command_buffer);
    last_executed = false;
}

} // namespace hydra::hw::tegra_x1::gpu::capture

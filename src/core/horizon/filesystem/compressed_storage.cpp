#include "core/horizon/filesystem/compressed_storage.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hydra::horizon::filesystem {

namespace {

constexpr u64 NODE_SIZE = 0x4000;
constexpr u64 NODE_HEADER_SIZE = 0x10;
constexpr u64 ENTRY_SIZE = 0x18;
constexpr u64 ENTRIES_PER_SET = (NODE_SIZE - NODE_HEADER_SIZE) / ENTRY_SIZE;
constexpr u64 OFFSETS_PER_NODE = (NODE_SIZE - NODE_HEADER_SIZE) / sizeof(u64);
// A decompressed block is held in memory while it is read
constexpr u64 MAX_BLOCK_SIZE = 16 * 1024 * 1024;

enum class CompressionType : u8 {
    None = 0,
    Zeros = 1,
    Lz4 = 3,
};

struct Entry {
    u64 virtual_offset;
    u64 virtual_size;
    u64 physical_offset;
    u32 physical_size;
    CompressionType type;
};

template <typename T>
T at(std::span<const u8> bytes, u64 offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("Truncated NCA compression table.");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct State {
    std::shared_ptr<IFile> section;
    u64 size;
    std::vector<Entry> entries;
};

class CompressedStream final : public ztd::io::IStream {
  public:
    explicit CompressedStream(std::shared_ptr<State> state_)
        : state{std::move(state_)},
          source{state->section->open(FileOpenFlags::Read)} {}

    u64 getSeek() const noexcept override { return position; }
    u64 getSize() const noexcept override { return state->size; }
    void seekTo(u64 seek) noexcept override { position = seek; }
    void seekBy(u64 amount) noexcept override { position += amount; }

  protected:
    void readRaw(std::span<u8> output) noexcept override {
        ASSERT(position <= state->size &&
                   output.size() <= state->size - position,
               Filesystem, "Compressed NCA read exceeds the section");
        try {
            while (!output.empty()) {
                const auto next = std::upper_bound(
                    state->entries.begin(), state->entries.end(), position,
                    [](u64 p, const Entry& e) { return p < e.virtual_offset; });
                const auto& entry = *std::prev(next);
                const auto skip = position - entry.virtual_offset;
                const auto count =
                    std::min<u64>(output.size(), entry.virtual_size - skip);
                auto chunk = output.first(count);
                switch (entry.type) {
                case CompressionType::None:
                    source->seekTo(entry.physical_offset + skip);
                    source->readToSpan(chunk);
                    break;
                case CompressionType::Zeros:
                    std::fill(chunk.begin(), chunk.end(), u8{0});
                    break;
                case CompressionType::Lz4:
                    std::memcpy(chunk.data(), decompress(entry).data() + skip,
                                count);
                    break;
                }
                position += count;
                output = output.subspan(count);
            }
        } catch (const std::exception& error) {
            LOG_FATAL(Filesystem, "Failed to read compressed NCA: {}",
                      error.what());
        }
    }

  private:
    std::shared_ptr<State> state;
    std::unique_ptr<ztd::io::IStream> source;
    u64 position{0};
    // The last decompressed block: reads are mostly sequential
    const Entry* cached_entry{nullptr};
    std::vector<u8> cached_block;
    std::vector<u8> compressed;

    std::span<const u8> decompress(const Entry& entry) {
        if (cached_entry == &entry)
            return cached_block;
        compressed.resize(entry.physical_size);
        source->seekTo(entry.physical_offset);
        source->readToSpan(std::span(compressed));
        cached_block.resize(entry.virtual_size);
        cached_entry = nullptr;
        if (!decompressLz4Block(compressed, cached_block))
            throw std::runtime_error("Corrupt LZ4 block.");
        cached_entry = &entry;
        return cached_block;
    }
};

class CompressedFile final : public IFile {
  public:
    explicit CompressedFile(std::shared_ptr<State> state_)
        : state{std::move(state_)} {}

    ztd::io::IStream* open(FileOpenFlags flags) override {
        ASSERT(!any(flags & FileOpenFlags::Write), Filesystem,
               "Compressed NCA sections are read-only");
        return new CompressedStream(state);
    }
    u64 getSize() const override { return state->size; }

  private:
    std::shared_ptr<State> state;
};

} // namespace

std::optional<CompressionInfo>
readCompressionInfo(std::span<const u8> fs_header) {
    // CompressionInfo: a BucketTreeInfo (table offset, size and a BKTR
    // header) at 0x178
    constexpr u64 OFFSET = 0x178;
    const auto table_offset = at<u64>(fs_header, OFFSET);
    const auto table_size = at<u64>(fs_header, OFFSET + 0x8);
    if (table_size == 0)
        return std::nullopt;
    if (at<u32>(fs_header, OFFSET + 0x10) != makeMagic4('B', 'K', 'T', 'R') ||
        at<u32>(fs_header, OFFSET + 0x14) != 1)
        throw std::runtime_error("Invalid NCA compression table header.");
    return CompressionInfo{table_offset, table_size,
                           at<u32>(fs_header, OFFSET + 0x18)};
}

std::shared_ptr<IFile> openCompressedSection(std::shared_ptr<IFile> section,
                                             const CompressionInfo& info) {
    const auto section_size = section->getSize();
    if (info.entry_count == 0 || info.table_offset > section_size ||
        info.table_size > section_size - info.table_offset)
        throw std::runtime_error("NCA compression table out of its section.");
    const u64 set_count =
        (u64(info.entry_count) + ENTRIES_PER_SET - 1) / ENTRIES_PER_SET;
    // One level of offset nodes holds this many entry sets; deeper trees
    // (millions of entries) are not used by games
    if (set_count > OFFSETS_PER_NODE)
        throw std::runtime_error("NCA compression table is too deep.");
    if ((1 + set_count) * NODE_SIZE > info.table_size)
        throw std::runtime_error("Truncated NCA compression table.");

    std::vector<u8> table((1 + set_count) * NODE_SIZE);
    {
        std::unique_ptr<ztd::io::IStream> stream(
            section->open(FileOpenFlags::Read));
        stream->seekTo(info.table_offset);
        stream->readToSpan(std::span(table));
    }
    // Offset node: index, entry set count, end of the virtual storage, then
    // where each entry set begins
    const auto node = std::span<const u8>(table).first(NODE_SIZE);
    if (at<u32>(node, 0) != 0 || at<u32>(node, 4) != set_count)
        throw std::runtime_error("Invalid NCA compression offset node.");
    const auto virtual_size = at<u64>(node, 8);

    auto state = std::make_shared<State>();
    state->section = std::move(section);
    state->size = virtual_size;
    state->entries.reserve(info.entry_count);
    for (u64 set = 0; set < set_count; set++) {
        const auto entries =
            std::span<const u8>(table).subspan((1 + set) * NODE_SIZE, NODE_SIZE);
        const auto count = at<u32>(entries, 4);
        const auto set_begin = at<u64>(node, NODE_HEADER_SIZE + set * 8);
        const auto set_end = at<u64>(entries, 8);
        const auto expected_count =
            std::min<u64>(ENTRIES_PER_SET,
                          info.entry_count - set * ENTRIES_PER_SET);
        if (at<u32>(entries, 0) != set || count != expected_count ||
            set_end > virtual_size ||
            (set + 1 < set_count
                 ? set_end != at<u64>(node, NODE_HEADER_SIZE + (set + 1) * 8)
                 : set_end != virtual_size))
            throw std::runtime_error("Invalid NCA compression entry set.");
        for (u32 i = 0; i < count; i++) {
            const auto p = NODE_HEADER_SIZE + i * ENTRY_SIZE;
            const auto virtual_offset = at<u64>(entries, p);
            const auto physical_offset = at<u64>(entries, p + 0x8);
            const auto type = static_cast<CompressionType>(at<u8>(entries, p + 0x10));
            const auto physical_size = at<u32>(entries, p + 0x14);
            const auto next_offset =
                i + 1 < count ? at<u64>(entries, p + ENTRY_SIZE) : set_end;
            if ((i == 0 && virtual_offset != set_begin) ||
                (state->entries.empty() && virtual_offset != 0) ||
                next_offset <= virtual_offset)
                throw std::runtime_error("Unsorted NCA compression entries.");
            const auto size = next_offset - virtual_offset;
            // Stored blocks are as long as their range; games leave their
            // physical size at -1
            const u64 stored_size =
                type == CompressionType::None ? size : physical_size;
            switch (type) {
            case CompressionType::None:
                break;
            case CompressionType::Zeros:
                break;
            case CompressionType::Lz4:
                if (size > MAX_BLOCK_SIZE)
                    throw std::runtime_error("LZ4 NCA block is too large.");
                break;
            default:
                throw std::runtime_error(fmt::format(
                    "Unsupported NCA compression type {}", u32(type)));
            }
            if (type != CompressionType::Zeros &&
                (physical_offset > state->section->getSize() ||
                 stored_size > state->section->getSize() - physical_offset))
                throw std::runtime_error("NCA compressed block out of section.");
            state->entries.push_back({virtual_offset, size, physical_offset,
                                      static_cast<u32>(physical_size), type});
        }
    }
    if (state->entries.size() != info.entry_count)
        throw std::runtime_error("NCA compression entry count mismatch.");
    return std::make_shared<CompressedFile>(std::move(state));
}

bool decompressLz4Block(std::span<const u8> src, std::span<u8> dst) {
    usize in = 0;
    usize out = 0;
    const auto length = [&](usize base, usize& value) {
        value = base;
        if (base != 0xf)
            return true;
        while (true) {
            if (in >= src.size())
                return false;
            const u8 byte = src[in++];
            value += byte;
            if (byte != 0xff)
                return true;
        }
    };
    while (in < src.size()) {
        const u8 token = src[in++];
        usize literals;
        if (!length(token >> 4, literals) || literals > src.size() - in ||
            literals > dst.size() - out)
            return false;
        std::memcpy(dst.data() + out, src.data() + in, literals);
        in += literals;
        out += literals;
        // The last sequence has literals only
        if (in == src.size())
            break;
        if (src.size() - in < 2)
            return false;
        const usize back = src[in] | (usize(src[in + 1]) << 8);
        in += 2;
        usize match;
        if (back == 0 || back > out || !length(token & 0xf, match))
            return false;
        match += 4;
        if (match > dst.size() - out)
            return false;
        // Matches may overlap what they produce
        for (usize i = 0; i < match; i++, out++)
            dst[out] = dst[out - back];
    }
    return out == dst.size();
}

} // namespace hydra::horizon::filesystem

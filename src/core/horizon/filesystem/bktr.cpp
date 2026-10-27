#include "core/horizon/filesystem/bktr.hpp"
#include "core/horizon/filesystem/memory_file.hpp"

#include <algorithm>
#include <stdexcept>

namespace hydra::horizon::filesystem {
namespace {
template <typename T>
T at(std::span<const u8> bytes, usize offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("Truncated BKTR table.");
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}
void range(u64 offset, u64 size, u64 limit) {
    if (offset > limit || size > limit - offset)
        throw std::runtime_error("Invalid NCA patch range.");
}
struct Segment {
    u64 offset, size, source_offset;
    std::shared_ptr<IFile> source;
};
struct MappedState {
    u64 size;
    std::vector<Segment> segments;
};
class MappedStream final : public ztd::io::IStream {
  public:
    explicit MappedStream(std::shared_ptr<MappedState> state_)
        : state{std::move(state_)} {
        for (const auto& s : state->segments)
            if (!streams.contains(s.source.get()))
                streams.emplace(s.source.get(),
                                std::unique_ptr<ztd::io::IStream>(
                                    s.source->open(FileOpenFlags::Read)));
    }
    u64 getSeek() const noexcept override { return position; }
    u64 getSize() const noexcept override { return state->size; }
    void seekTo(u64 p) noexcept override { position = p; }
    void seekBy(u64 n) noexcept override { position += n; }

  protected:
    void readRaw(std::span<u8> output) noexcept override {
        ASSERT(position <= state->size &&
                   output.size() <= state->size - position,
               Filesystem, "NCA patch read exceeds bounds");
        while (!output.empty()) {
            const auto next = std::upper_bound(
                state->segments.begin(), state->segments.end(), position,
                [](u64 p, const Segment& s) { return p < s.offset; });
            const Segment* s =
                next == state->segments.begin() ? nullptr : &*std::prev(next);
            if (s && position < s->offset + s->size) {
                const auto n = std::min<u64>(output.size(),
                                             s->offset + s->size - position);
                auto& stream = streams.at(s->source.get());
                stream->seekTo(s->source_offset + position - s->offset);
                stream->readToSpan(output.first(n));
                output = output.subspan(n);
                position += n;
            } else {
                const auto end =
                    next == state->segments.end() ? state->size : next->offset;
                const auto n = std::min<u64>(output.size(), end - position);
                std::fill_n(output.data(), n, 0);
                output = output.subspan(n);
                position += n;
            }
        }
    }

  private:
    std::shared_ptr<MappedState> state;
    std::map<IFile*, std::unique_ptr<ztd::io::IStream>> streams;
    u64 position{};
};
class MappedFile final : public IFile {
  public:
    std::shared_ptr<MappedState> state = std::make_shared<MappedState>();
    ztd::io::IStream* open(FileOpenFlags flags) override {
        ASSERT(!any(flags & FileOpenFlags::Write), Filesystem,
               "Patch is read-only");
        return new MappedStream(state);
    }
    u64 getSize() const override { return state->size; }
};
NcaFile::Header headerOf(IFile& file) {
    range(0, 0xc00, file.getSize());
    NcaFile::Header h;
    std::unique_ptr<ztd::io::IStream> stream(file.open(FileOpenFlags::Read));
    stream->readToSpan(std::span<u8>(h));
    if (at<u32>(h, 0x200) != makeMagic4('N', 'C', 'A', '3'))
        throw std::runtime_error("Patch expects a decrypted NCA view.");
    return h;
}
template <typename T>
void put(NcaFile::Header& h, usize o, T v) {
    std::memcpy(h.data() + o, &v, sizeof(v));
}
} // namespace

BktrTable readBktrTable(IFile& file, u64 offset, u64 size, u32 entry_count,
                        bool relocation) {
    range(offset, size, file.getSize());
    if (size < 0x8000 || size > 64 * 1024 * 1024 || size % 0x4000 != 0)
        throw std::runtime_error("Unsupported BKTR table size.");
    std::vector<u8> data(size);
    std::unique_ptr<ztd::io::IStream> stream(file.open(FileOpenFlags::Read));
    stream->seekTo(offset);
    stream->readToSpan(std::span(data));
    const auto buckets = at<u32>(data, 4);
    if (buckets == 0 || buckets > 0x3ff0 / 8 ||
        size != (u64(buckets) + 1) * 0x4000)
        throw std::runtime_error(
            "Unsupported BKTR tree depth or bucket count.");
    BktrTable table{at<u64>(data, 8), {}};
    const u64 stride = relocation ? 20 : 16;
    for (u32 b = 0; b < buckets; b++) {
        const u64 node = (u64(b) + 1) * 0x4000;
        const auto count = at<u32>(data, node + 4);
        const auto end = at<u64>(data, node + 8);
        const auto expected_end =
            b + 1 < buckets ? at<u64>(data, 16 + (b + 1) * 8) : table.end;
        if (!count || count > 0x3ff0 / stride || end != expected_end)
            throw std::runtime_error("Invalid BKTR leaf bounds.");
        for (u32 i = 0; i < count; i++) {
            const auto p = node + 16 + i * stride;
            const auto start = at<u64>(data, p);
            if ((table.entries.empty() && start != 0) ||
                (!table.entries.empty() &&
                 start <= table.entries.back().offset) ||
                start >= end || (i == 0 && start != at<u64>(data, 16 + b * 8)))
                throw std::runtime_error("Unsorted or incomplete BKTR table.");
            const auto value =
                relocation ? at<u64>(data, p + 8) : u64(at<u32>(data, p + 12));
            const auto source =
                relocation ? at<u32>(data, p + 16) : at<u32>(data, p + 8);
            if (source > 1 || (!relocation && start % 16 != 0))
                throw std::runtime_error("Unsupported BKTR entry.");
            table.entries.push_back({start, value, source});
        }
    }
    if (table.entries.size() != entry_count)
        throw std::runtime_error("BKTR entry count mismatch.");
    return table;
}

std::shared_ptr<IFile> applyNcaPatch(std::shared_ptr<IFile> base,
                                     std::shared_ptr<IFile> patch) {
    const auto base_header = headerOf(*base);
    auto h = headerOf(*patch);
    if (h[0x205] != 0 || base_header[0x205] != 0 ||
        at<u64>(h, 0x210) != at<u64>(base_header, 0x210))
        throw std::runtime_error(
            "Update Program NCA does not match this base game.");
    auto file = std::make_shared<MappedFile>();
    u64 cursor = h.size();
    for (usize i = 0; i < 4; i++) {
        const auto fs = 0x400 + i * 0x200;
        const u64 start = u64(at<u32>(h, 0x240 + i * 16)) * 0x200;
        const u64 end = u64(at<u32>(h, 0x244 + i * 16)) * 0x200;
        if (!start)
            continue;
        range(start, end >= start ? end - start : ~u64{}, patch->getSize());
        u64 virtual_size = end - start;
        const bool bktr = at<u64>(h, fs + 0x108) != 0;
        if (bktr) {
            if (at<u32>(h, fs + 0x110) != makeMagic4('B', 'K', 'T', 'R') ||
                at<u32>(h, fs + 0x114) != 1)
                throw std::runtime_error("Unsupported BKTR relocation header.");
            const u64 ro = at<u64>(h, fs + 0x100), rs = at<u64>(h, fs + 0x108);
            range(ro, rs, end - start);
            const auto table = readBktrTable(*patch, start + ro, rs,
                                             at<u32>(h, fs + 0x118), true);
            virtual_size = table.end;
            if (virtual_size > (u64(0xffffffff) * 0x200) - cursor)
                throw std::runtime_error(
                    "NCA patch virtual section is too large.");
            const u64 bs = u64(at<u32>(base_header, 0x240 + i * 16)) * 0x200;
            const u64 be = u64(at<u32>(base_header, 0x244 + i * 16)) * 0x200;
            if (!bs || be < bs)
                throw std::runtime_error(
                    "Update needs a missing base NCA section.");
            for (usize j = 0; j < table.entries.size(); j++) {
                const auto& e = table.entries[j];
                const auto n =
                    (j + 1 < table.entries.size() ? table.entries[j + 1].offset
                                                  : table.end) -
                    e.offset;
                range(e.value, n, e.source ? ro : be - bs);
                file->state->segments.push_back(
                    {cursor + e.offset, n, (e.source ? start : bs) + e.value,
                     e.source ? patch : base});
            }
        } else {
            file->state->segments.push_back(
                {cursor, virtual_size, start, patch});
        }
        if (virtual_size > (u64(0xffffffff) * 0x200) - cursor)
            throw std::runtime_error("NCA patch virtual section is too large.");
        put(h, 0x240 + i * 16, u32(cursor / 0x200));
        cursor += (virtual_size + 0x1ff) & ~u64{0x1ff};
        put(h, 0x244 + i * 16, u32(cursor / 0x200));
        h[fs + 4] = 1;
        std::fill_n(h.data() + fs + 0x100, 0x40, 0);
    }
    put(h, 0x208, cursor);
    file->state->size = cursor;
    // MemoryFile is immutable backing for the synthetic, relocated NCA header.
    auto header_file =
        std::make_shared<MemoryFile>(std::vector<u8>(h.begin(), h.end()));
    file->state->segments.insert(file->state->segments.begin(),
                                 {0, h.size(), 0, header_file});
    return file;
}
} // namespace hydra::horizon::filesystem

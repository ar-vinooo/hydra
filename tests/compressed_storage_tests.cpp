#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#include "core/horizon/filesystem/compressed_storage.hpp"
#include "core/horizon/filesystem/memory_file.hpp"
#include "core/horizon/loader/loader.hpp"

using namespace hydra;
using namespace hydra::horizon::filesystem;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename T>
void put(std::vector<u8>& bytes, usize offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// LZ4 sequence: literals, then a match of `match` bytes `back` behind
void lz4Sequence(std::vector<u8>& out, std::span<const u8> literals,
                 usize back = 0, usize match = 0) {
    const auto extend = [&](usize value) {
        for (value -= 15; value >= 255; value -= 255)
            out.push_back(255);
        out.push_back(static_cast<u8>(value));
    };
    const usize literal_nibble = std::min<usize>(literals.size(), 15);
    const usize match_nibble = match ? std::min<usize>(match - 4, 15) : 0;
    out.push_back(static_cast<u8>((literal_nibble << 4) | match_nibble));
    if (literals.size() >= 15)
        extend(literals.size());
    out.insert(out.end(), literals.begin(), literals.end());
    if (match == 0)
        return;
    out.push_back(static_cast<u8>(back));
    out.push_back(static_cast<u8>(back >> 8));
    if (match - 4 >= 15)
        extend(match - 4);
}

// A block of `size` bytes as LZ4: a literal run, a repeating match and a
// final literal run
std::vector<u8> lz4Block(std::span<const u8> data) {
    std::vector<u8> out;
    if (data.size() < 32) {
        lz4Sequence(out, data);
        return out;
    }
    // The block repeats its first 7 bytes until the last 8, which the test
    // data below guarantees
    lz4Sequence(out, data.first(7), 7, data.size() - 7 - 8);
    lz4Sequence(out, data.last(8));
    return out;
}

void testLz4() {
    const std::vector<u8> text{'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c',
                               'a', 'b', 'c', 'x', 'y', 'z', '!', '?'};
    std::vector<u8> encoded;
    lz4Sequence(encoded, std::span(text).first(3), 3, 9);
    lz4Sequence(encoded, std::span(text).subspan(12));
    std::vector<u8> decoded(text.size());
    check(decompressLz4Block(encoded, decoded) && decoded == text,
          "Overlapping LZ4 matches decode");

    std::vector<u8> long_literals(300);
    for (usize i = 0; i < long_literals.size(); i++)
        long_literals[i] = static_cast<u8>(i * 7);
    std::vector<u8> literal_block;
    lz4Sequence(literal_block, long_literals);
    std::vector<u8> literal_out(long_literals.size());
    check(decompressLz4Block(literal_block, literal_out) &&
              literal_out == long_literals,
          "Extended literal lengths decode");

    std::vector<u8> out(text.size());
    auto zero_offset = encoded;
    zero_offset[4] = 0;
    zero_offset[5] = 0;
    check(!decompressLz4Block(zero_offset, out), "A zero match offset fails");
    auto far = encoded;
    far[4] = 0xff;
    check(!decompressLz4Block(far, out), "A match before the output fails");
    check(!decompressLz4Block(std::span(encoded).first(5), out),
          "A truncated block fails");
    std::vector<u8> small(text.size() - 1);
    check(!decompressLz4Block(encoded, small), "An overflowing block fails");
    std::vector<u8> large(text.size() + 1);
    check(!decompressLz4Block(encoded, large), "A short block fails");
    std::cout << "PASS: bounded LZ4 block decoding\n";
}

struct Layout {
    std::vector<u8> section;
    std::vector<u8> expected;
    CompressionInfo info;
};

// Builds a section of `count` entries cycling through stored, zero and LZ4
// blocks, followed by its bucket tree
Layout makeSection(u32 count) {
    constexpr usize NODE = 0x4000, ENTRY = 0x18;
    constexpr usize PER_SET = (NODE - 0x10) / ENTRY;
    Layout layout;
    std::mt19937 rng(1234);
    struct Block {
        u64 virtual_offset, physical_offset;
        u32 physical_size;
        u8 type;
    };
    std::vector<Block> blocks;
    for (u32 i = 0; i < count; i++) {
        const usize size = 32 + rng() % 96;
        std::vector<u8> data(size);
        for (auto& byte : data)
            byte = static_cast<u8>(rng());
        const u8 type = i % 3 == 0 ? 0 : i % 3 == 1 ? 1 : 3;
        if (type == 1)
            std::fill(data.begin(), data.end(), u8{0});
        if (type == 3)
            for (usize j = 7; j + 8 < size; j++)
                data[j] = data[j - 7];
        Block block{layout.expected.size(), layout.section.size(), 0, type};
        layout.expected.insert(layout.expected.end(), data.begin(), data.end());
        if (type == 0) {
            layout.section.insert(layout.section.end(), data.begin(), data.end());
            // As in games: stored blocks leave their physical size at -1
            block.physical_size = 0xffffffff;
        } else if (type == 3) {
            const auto encoded = lz4Block(data);
            layout.section.insert(layout.section.end(), encoded.begin(),
                                  encoded.end());
            block.physical_size = static_cast<u32>(encoded.size());
        }
        blocks.push_back(block);
    }
    const usize sets = (count + PER_SET - 1) / PER_SET;
    const usize table_offset = (layout.section.size() + 0xfff) & ~usize{0xfff};
    layout.section.resize(table_offset + (1 + sets) * NODE);
    const auto virtual_size = layout.expected.size();
    put<u32>(layout.section, table_offset, 0);
    put<u32>(layout.section, table_offset + 4, static_cast<u32>(sets));
    put<u64>(layout.section, table_offset + 8, virtual_size);
    for (usize set = 0; set < sets; set++) {
        const usize first = set * PER_SET;
        const usize last = std::min<usize>(count, first + PER_SET);
        put<u64>(layout.section, table_offset + 0x10 + set * 8,
                 blocks[first].virtual_offset);
        const usize node = table_offset + (1 + set) * NODE;
        put<u32>(layout.section, node, static_cast<u32>(set));
        put<u32>(layout.section, node + 4, static_cast<u32>(last - first));
        put<u64>(layout.section, node + 8,
                 last < count ? blocks[last].virtual_offset : virtual_size);
        for (usize i = first; i < last; i++) {
            const usize p = node + 0x10 + (i - first) * ENTRY;
            put<u64>(layout.section, p, blocks[i].virtual_offset);
            put<u64>(layout.section, p + 8, blocks[i].physical_offset);
            layout.section[p + 0x10] = blocks[i].type;
            put<u32>(layout.section, p + 0x14, blocks[i].physical_size);
        }
    }
    layout.info = {table_offset, (1 + sets) * NODE, count};
    return layout;
}

std::shared_ptr<IFile> open(const Layout& layout) {
    return openCompressedSection(std::make_shared<MemoryFile>(layout.section),
                                 layout.info);
}

void testSection() {
    // More entries than one entry set holds
    const auto layout = makeSection(1500);
    const auto file = open(layout);
    check(file->getSize() == layout.expected.size(),
          "The virtual size comes from the tree");
    std::unique_ptr<ztd::io::IStream> stream(file->open(FileOpenFlags::Read));
    std::vector<u8> all(layout.expected.size());
    stream->readToSpan(std::span(all));
    check(all == layout.expected, "A sequential read decompresses every block");
    std::mt19937 rng(99);
    for (u32 i = 0; i < 2000; i++) {
        const u64 offset = rng() % layout.expected.size();
        const u64 size = 1 + rng() % std::min<u64>(400, layout.expected.size() - offset);
        std::vector<u8> part(size);
        stream->seekTo(offset);
        stream->readToSpan(std::span(part));
        check(std::equal(part.begin(), part.end(),
                         layout.expected.begin() + static_cast<std::ptrdiff_t>(offset)),
              "Random reads across blocks and entry sets match");
    }
    std::cout << "PASS: compressed NCA sections with stored, zero and LZ4 "
                 "blocks across entry sets\n";
}

void expectRejected(Layout layout, const char* message) {
    bool rejected = false;
    try {
        open(layout);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, message);
}

void testMalformed() {
    const auto base = makeSection(800);
    const auto table = base.info.table_offset;
    {
        auto layout = base;
        layout.info.entry_count += 1;
        expectRejected(layout, "An entry count mismatch is rejected");
    }
    {
        auto layout = base;
        put<u32>(layout.section, table + 4, 7);
        expectRejected(layout, "A wrong entry set count is rejected");
    }
    {
        auto layout = base;
        layout.section[table + 0x4000 + 0x10 + 2 * 0x18 + 0x10] = 2;
        expectRejected(layout, "An unknown compression type is rejected");
    }
    {
        auto layout = base;
        put<u64>(layout.section, table + 0x4000 + 0x10 + 3 * 0x18,
                 u64{0});
        expectRejected(layout, "Unsorted entries are rejected");
    }
    {
        auto layout = base;
        put<u64>(layout.section, table + 0x4000 + 0x10 + 0x8, u64{1} << 40);
        expectRejected(layout, "A block outside the section is rejected");
    }
    {
        auto layout = base;
        layout.info.table_size = 0x4000;
        expectRejected(layout, "A truncated table is rejected");
    }
    std::cout << "PASS: malformed compression tables are rejected\n";
}

} // namespace

// Optional: loads a real package the way Hydra opens a game (without
// starting it) and reads its whole RomFS, which decompresses every block of
// a compressed section
void testPackage(const char* path) {
    auto loader = horizon::loader::ILoader::createFromPath(path);
    check(loader.has_value(), "The package loads");
    auto* entry = (*loader)->getRomFs();
    check(entry != nullptr && entry->isFile(), "The package has a RomFS file");
    auto* romfs = static_cast<IFile*>(entry);
    std::unique_ptr<ztd::io::IStream> stream(romfs->open(FileOpenFlags::Read));
    std::vector<u8> chunk(1024 * 1024);
    u64 checksum = 0;
    for (u64 offset = 0; offset < romfs->getSize(); offset += chunk.size()) {
        const auto size = std::min<u64>(chunk.size(), romfs->getSize() - offset);
        stream->readToSpan(std::span(chunk).first(size));
        for (u64 i = 0; i < size; i += 4096)
            checksum = checksum * 31 + chunk[i];
    }
    std::vector<u8> header(0x50);
    stream->seekTo(0);
    stream->readToSpan(std::span(header));
    u64 header_size;
    std::memcpy(&header_size, header.data(), sizeof(header_size));
    check(header_size == 0x50, "The RomFS header is readable");
    std::cout << "PASS: " << path << ": read " << romfs->getSize()
              << " RomFS bytes (sample checksum " << std::hex << checksum
              << std::dec << ")\n";
}

int main(int argc, char** argv) {
    testLz4();
    testSection();
    testMalformed();
    if (argc > 1)
        testPackage(argv[1]);
    return 0;
}

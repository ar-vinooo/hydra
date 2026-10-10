#include "core/horizon/loader/xci_loader.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "core/horizon/filesystem/disk_file.hpp"
#include "core/horizon/filesystem/file_view.hpp"
#include "core/horizon/loader/npdm.hpp"

namespace hydra::horizon::loader {
namespace {

using filesystem::IFile;
using filesystem::NcaFile;

template <typename T>
T valueAt(std::span<const u8> data, usize offset) {
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("Truncated XCI metadata.");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

void checkRange(u64 offset, u64 size, u64 limit) {
    if (offset > limit || size > limit - offset)
        throw std::runtime_error(
            "XCI contains an invalid file or section range.");
}

std::vector<u8> read(IFile& file, u64 offset, u64 size) {
    checkRange(offset, size, file.getSize());
    if (size > 16 * 1024 * 1024)
        throw std::runtime_error(
            "XCI metadata exceeds the supported size limit.");
    std::unique_ptr<ztd::io::IStream> stream(
        file.open(filesystem::FileOpenFlags::Read));
    stream->seekTo(offset);
    std::vector<u8> data(size);
    stream->readToSpan(std::span(data));
    return data;
}

struct Entry {
    std::string name;
    u64 offset;
    u64 size;
};

std::vector<Entry> partition(IFile& file, u64 offset, u64 size, bool hashed) {
    checkRange(offset, size, file.getSize());
    if (size < 16)
        throw std::runtime_error("Truncated XCI partition.");
    const auto header = read(file, offset, 16);
    const auto magic = hashed ? makeMagic4('H', 'F', 'S', '0')
                              : makeMagic4('P', 'F', 'S', '0');
    if (valueAt<u32>(header, 0) != magic)
        throw std::runtime_error(
            "Invalid XCI partition magic or incorrect NCA keys.");
    const auto count = valueAt<u32>(header, 4);
    if (count > 4096)
        throw std::runtime_error("XCI partition has too many entries.");
    const auto strings_size = valueAt<u32>(header, 8);
    const u64 entry_size = hashed ? 0x40 : 0x18;
    const auto table_size = static_cast<u64>(count) * entry_size;
    const auto data_offset = 16 + table_size + strings_size;
    checkRange(0, data_offset, size);
    const auto table = read(file, offset + 16, table_size);
    const auto strings = read(file, offset + 16 + table_size, strings_size);
    std::vector<Entry> result;
    std::set<std::string> names;
    for (u32 i = 0; i < count; ++i) {
        const auto entry = static_cast<usize>(i * entry_size);
        const auto start = valueAt<u64>(table, entry);
        const auto length = valueAt<u64>(table, entry + 8);
        const auto string_offset = valueAt<u32>(table, entry + 16);
        checkRange(start, length, size - data_offset);
        if (string_offset >= strings.size())
            throw std::runtime_error("Invalid XCI partition filename offset.");
        const auto begin = strings.begin() + string_offset;
        const auto end = std::find(begin, strings.end(), 0);
        if (end == strings.end())
            throw std::runtime_error("Unterminated XCI partition filename.");
        std::string name(begin, end);
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos ||
            !names.insert(name).second)
            throw std::runtime_error(
                "Invalid or duplicate XCI partition filename.");
        if (hashed && valueAt<u32>(table, entry + 20) > length)
            throw std::runtime_error("Invalid XCI hashed region size.");
        for (const auto& previous : result) {
            const auto absolute = offset + data_offset + start;
            if (length != 0 && previous.size != 0 &&
                absolute < previous.offset + previous.size &&
                absolute + length > previous.offset)
                throw std::runtime_error("Overlapping XCI partition entries.");
        }
        result.push_back(
            {std::move(name), offset + data_offset + start, length});
    }
    return result;
}

struct Region {
    u64 offset;
    u64 size;
};

Region sectionRegion(const NcaFile::Header& header, usize index, bool romfs) {
    const auto start =
        static_cast<u64>(valueAt<u32>(header, 0x240 + index * 16)) * 0x200;
    const auto end =
        static_cast<u64>(valueAt<u32>(header, 0x244 + index * 16)) * 0x200;
    if (start == 0 || end <= start)
        throw std::runtime_error("Required XCI NCA section is missing.");
    const auto fs = 0x400 + index * 0x200;
    u64 offset;
    u64 size;
    if (romfs) {
        if (header[fs + 2] != 0 || header[fs + 3] != 3 ||
            valueAt<u32>(header, fs + 8) != makeMagic4('I', 'V', 'F', 'C') ||
            valueAt<u32>(header, fs + 0x14) != 7)
            throw std::runtime_error("Unsupported XCI RomFS layout.");
        offset = valueAt<u64>(header, fs + 0x90);
        size = valueAt<u64>(header, fs + 0x98);
    } else {
        if (header[fs + 2] != 1 || header[fs + 3] != 2 ||
            valueAt<u32>(header, fs + 0x2c) != 2)
            throw std::runtime_error("Unsupported XCI ExeFS layout.");
        offset = valueAt<u64>(header, fs + 0x40);
        size = valueAt<u64>(header, fs + 0x48);
    }
    checkRange(offset, size, end - start);
    return {start + offset, size};
}

void validateProgram(NcaFile& file, const NcaFile::Header& header) {
    const auto code = sectionRegion(header, 0, false);
    const auto entries = partition(file, code.offset, code.size, false);
    const auto npdm = std::ranges::find(entries, "main.npdm", &Entry::name);
    if (npdm == entries.end() || npdm->size < sizeof(NpdmMeta))
        throw std::runtime_error("XCI program is missing main.npdm.");
    const auto meta = read(file, npdm->offset, sizeof(NpdmMeta));
    if (valueAt<u32>(meta, 0) != makeMagic4('M', 'E', 'T', 'A'))
        throw std::runtime_error("Cannot decrypt XCI program metadata. Check "
                                 "the key-area keys in prod.keys.");
    if ((meta[0xc] & 1) == 0)
        throw std::runtime_error("32-bit XCI programs are not supported.");
    if (meta[0xf] >= 4 || meta[0xe] > 63 || meta[0x2f] != 0)
        throw std::runtime_error("Invalid XCI program metadata.");
    bool has_main = false;
    for (const auto& entry : entries) {
        if (entry.name == "main.npdm")
            continue;
        if (entry.size < 0x100 ||
            valueAt<u32>(read(file, entry.offset, 4), 0) !=
                makeMagic4('N', 'S', 'O', '0'))
            throw std::runtime_error("Invalid executable in XCI ExeFS.");
        has_main |= entry.name == "main";
    }
    if (!has_main)
        throw std::runtime_error("XCI program is missing its main executable.");
    const auto data = sectionRegion(header, 1, true);
    const auto romfs = read(file, data.offset, 0x50);
    if (valueAt<u64>(romfs, 0) != 0x50)
        throw std::runtime_error("Cannot decrypt XCI RomFS. Check prod.keys.");
    // Validate the logo PFS before the existing NCA loader reads it.
    if (valueAt<u32>(header, 0x260) != 0) {
        const auto logo = sectionRegion(header, 2, false);
        (void)partition(file, logo.offset, logo.size, false);
    }
}

std::vector<Entry> controlEntries(NcaFile& file,
                                  const NcaFile::Header& header) {
    const auto data = sectionRegion(header, 0, true);
    const auto romfs = read(file, data.offset, 0x50);
    if (valueAt<u64>(romfs, 0) != 0x50)
        throw std::runtime_error("Invalid XCI control RomFS.");
    const auto dir_offset = valueAt<u64>(romfs, 0x18);
    const auto dir_size = valueAt<u64>(romfs, 0x20);
    const auto file_offset = valueAt<u64>(romfs, 0x38);
    const auto file_size = valueAt<u64>(romfs, 0x40);
    const auto content_offset = valueAt<u64>(romfs, 0x48);
    checkRange(dir_offset, dir_size, data.size);
    checkRange(file_offset, file_size, data.size);
    checkRange(content_offset, 0, data.size);
    if (dir_size < 0x18)
        throw std::runtime_error("Truncated XCI control directory.");
    const auto root = read(file, data.offset + dir_offset, 0x18);
    const auto files = read(file, data.offset + file_offset, file_size);
    std::set<u32> visited;
    std::vector<Entry> result;
    auto next = valueAt<u32>(root, 0xc);
    while (next != invalid<u32>()) {
        if (!visited.insert(next).second)
            throw std::runtime_error("Cyclic XCI control file table.");
        checkRange(next, 0x20, files.size());
        const auto name_size = valueAt<u32>(files, next + 0x1c);
        checkRange(static_cast<u64>(next) + 0x20, name_size, files.size());
        const auto offset = valueAt<u64>(files, next + 8);
        const auto size = valueAt<u64>(files, next + 16);
        checkRange(offset, size, data.size - content_offset);
        std::string name(
            reinterpret_cast<const char*>(files.data() + next + 0x20),
            name_size);
        result.push_back(
            {std::move(name), data.offset + content_offset + offset, size});
        next = valueAt<u32>(files, next + 4);
    }
    return result;
}

} // namespace

XciLoader::XciLoader(std::shared_ptr<NcaFile> program)
    : NcaLoader(program.get()), program_file{std::move(program)} {}

XciLoader* XciLoader::create(std::string_view path) {
    auto container = std::make_shared<filesystem::DiskFile>(path);
    const auto header = read(*container, 0, 0x200);
    if (valueAt<u32>(header, 0x100) != makeMagic4('H', 'E', 'A', 'D'))
        throw std::runtime_error("Invalid XCI gamecard header.");
    const auto root = valueAt<u64>(header, 0x130);
    checkRange(root, 16, container->getSize());
    const auto partitions =
        partition(*container, root, container->getSize() - root, true);
    const auto secure = std::ranges::find(partitions, "secure", &Entry::name);
    if (secure == partitions.end())
        throw std::runtime_error("XCI has no secure game partition.");
    const auto entries =
        partition(*container, secure->offset, secure->size, true);
    const auto keys = NcaFile::loadDefaultKeys();
    struct Content {
        Entry entry;
        NcaFile::Header header;
        bool plaintext;
    };
    std::vector<Content> content;
    for (const auto& entry : entries) {
        if (!std::string_view(entry.name).ends_with(".nca"))
            continue;
        bool plaintext;
        auto nca_header = NcaFile::readHeader(*container, entry.offset,
                                              entry.size, keys, plaintext);
        content.push_back({entry, nca_header, plaintext});
    }
    // Updates may carry the same program ID. Select a base Program NCA, never
    // the largest archive or an update that requires a BKTR overlay.
    const Content* program = nullptr;
    for (const auto& item : content) {
        if (item.header[0x205] != 0 ||
            (valueAt<u64>(item.header, 0x210) & 0x800) != 0)
            continue;
        bool patch = false;
        for (usize i = 0; i < 4; ++i) {
            if (valueAt<u32>(item.header, 0x240 + i * 16) != 0 &&
                item.header[0x404 + i * 0x200] == 4)
                patch = true;
        }
        if (patch)
            continue;
        if (program != nullptr)
            throw std::runtime_error(
                "This XCI contains multiple base games. Multi-game selection "
                "is not supported yet.");
        program = &item;
    }
    if (program == nullptr)
        throw std::runtime_error("XCI has no supported base Program NCA; "
                                 "update-only cartridges are not supported.");
    auto program_file = std::make_shared<NcaFile>(
        container, program->entry.offset, program->entry.size, program->header,
        program->plaintext, keys);
    validateProgram(*program_file, program->header);
    auto loader =
        std::unique_ptr<XciLoader>(new XciLoader(std::move(program_file)));
    // Match Control metadata by title ID; never use DLC/update metadata.
    for (const auto& item : content) {
        if (item.header[0x205] != 2 ||
            valueAt<u64>(item.header, 0x210) != loader->getTitleId())
            continue;
        try {
            NcaFile control(container, item.entry.offset, item.entry.size,
                            item.header, item.plaintext, keys);
            const auto files = controlEntries(control, item.header);
            for (const auto& file : files) {
                if (file.name == "control.nacp" &&
                    file.size ==
                        sizeof(services::ns::ApplicationControlProperty)) {
                    loader->control_file =
                        std::make_unique<filesystem::MemoryFile>(
                            read(control, file.offset, file.size));
                    loader->nacp_file = loader->control_file.get();
                } else if (std::string_view(file.name).starts_with("icon_") &&
                           std::string_view(file.name).ends_with(".dat") &&
                           !loader->game_icon) {
                    loader->game_icon =
                        std::make_unique<filesystem::MemoryFile>(
                            read(control, file.offset, file.size));
                    loader->icon_file = loader->game_icon.get();
                }
            }
            break;
        } catch (const std::exception& error) {
            LOG_WARN(Loader, "Could not load XCI control metadata: {}",
                     error.what());
        }
    }
    LOG_INFO(Loader, "Loaded XCI base game {:016x} directly from cartridge",
             loader->getTitleId());
    if (std::ranges::any_of(content, [](const Content& item) {
            return item.header[0x205] == 0 && item.header[0x604] == 4;
        }))
        LOG_WARN(
            Loader,
            "Bundled XCI updates/DLC are not applied; loading the base game");
    return loader.release();
}

} // namespace hydra::horizon::loader

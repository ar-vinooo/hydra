#include "core/horizon/loader/xci_loader.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <unistd.h>

#include "core/horizon/filesystem/bktr.hpp"
#include "core/horizon/filesystem/file_view.hpp"
#include "core/horizon/loader/content_package.hpp"
#include "core/system.hpp"

#include "core/horizon/filesystem/disk_file.hpp"
#include "core/horizon/filesystem/ncz_file.hpp"
#include "core/horizon/loader/npdm.hpp"

namespace hydra::horizon::loader {
namespace {

using filesystem::IFile;
using filesystem::NcaFile;

template <typename T>
T valueAt(std::span<const u8> data, usize offset) {
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("Truncated Package metadata.");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

void checkRange(u64 offset, u64 size, u64 limit) {
    if (offset > limit || size > limit - offset)
        throw std::runtime_error(
            "Package contains an invalid file or section range.");
}

std::vector<u8> read(IFile& file, u64 offset, u64 size) {
    checkRange(offset, size, file.getSize());
    if (size > 16 * 1024 * 1024)
        throw std::runtime_error(
            "Package metadata exceeds the supported size limit.");
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
        throw std::runtime_error("Truncated Package partition.");
    const auto header = read(file, offset, 16);
    const auto magic = hashed ? makeMagic4('H', 'F', 'S', '0')
                              : makeMagic4('P', 'F', 'S', '0');
    if (valueAt<u32>(header, 0) != magic)
        throw std::runtime_error(
            "Invalid Package partition magic or incorrect NCA keys.");
    const auto count = valueAt<u32>(header, 4);
    if (count > 4096)
        throw std::runtime_error("Package partition has too many entries.");
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
            throw std::runtime_error(
                "Invalid Package partition filename offset.");
        const auto begin = strings.begin() + string_offset;
        const auto end = std::find(begin, strings.end(), 0);
        if (end == strings.end())
            throw std::runtime_error(
                "Unterminated Package partition filename.");
        std::string name(begin, end);
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos ||
            !names.insert(name).second)
            throw std::runtime_error(
                "Invalid or duplicate Package partition filename.");
        if (hashed && valueAt<u32>(table, entry + 20) > length)
            throw std::runtime_error("Invalid XCI hashed region size.");
        for (const auto& previous : result) {
            const auto absolute = offset + data_offset + start;
            if (length != 0 && previous.size != 0 &&
                absolute < previous.offset + previous.size &&
                absolute + length > previous.offset)
                throw std::runtime_error(
                    "Overlapping Package partition entries.");
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
        throw std::runtime_error("Required Package NCA section is missing.");
    const auto fs = 0x400 + index * 0x200;
    u64 offset;
    u64 size;
    if (romfs) {
        if (header[fs + 2] != 0 || header[fs + 3] != 3 ||
            valueAt<u32>(header, fs + 8) != makeMagic4('I', 'V', 'F', 'C') ||
            valueAt<u32>(header, fs + 0x14) != 7)
            throw std::runtime_error("Unsupported Package RomFS layout.");
        offset = valueAt<u64>(header, fs + 0x90);
        size = valueAt<u64>(header, fs + 0x98);
    } else {
        if (header[fs + 2] != 1 || header[fs + 3] != 2 ||
            valueAt<u32>(header, fs + 0x2c) != 2)
            throw std::runtime_error("Unsupported Package ExeFS layout.");
        offset = valueAt<u64>(header, fs + 0x40);
        size = valueAt<u64>(header, fs + 0x48);
    }
    checkRange(offset, size, end - start);
    return {start + offset, size};
}

void validateProgram(IFile& file, const NcaFile::Header& header) {
    const auto code = sectionRegion(header, 0, false);
    const auto entries = partition(file, code.offset, code.size, false);
    const auto npdm = std::ranges::find(entries, "main.npdm", &Entry::name);
    if (npdm == entries.end() || npdm->size < sizeof(NpdmMeta))
        throw std::runtime_error("Package program is missing main.npdm.");
    const auto meta = read(file, npdm->offset, sizeof(NpdmMeta));
    if (valueAt<u32>(meta, 0) != makeMagic4('M', 'E', 'T', 'A'))
        throw std::runtime_error(
            "Cannot decrypt Package program metadata. Check "
            "the key-area keys in prod.keys.");
    if (meta[0xf] >= 4 || meta[0xe] > 63 || meta[0x2f] != 0)
        throw std::runtime_error("Invalid Package program metadata.");
    bool has_main = false;
    for (const auto& entry : entries) {
        if (entry.name == "main.npdm")
            continue;
        if (entry.size < 0x100 ||
            valueAt<u32>(read(file, entry.offset, 4), 0) !=
                makeMagic4('N', 'S', 'O', '0'))
            throw std::runtime_error("Invalid executable in Package ExeFS.");
        has_main |= entry.name == "main";
    }
    if (!has_main)
        throw std::runtime_error(
            "Package program is missing its main executable.");
    const auto data = sectionRegion(header, 1, true);
    const auto romfs = read(file, data.offset, 0x50);
    if (valueAt<u64>(romfs, 0) != 0x50)
        throw std::runtime_error(
            "Cannot decrypt Package RomFS. Check prod.keys.");
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
        throw std::runtime_error("Invalid Package control RomFS.");
    const auto dir_offset = valueAt<u64>(romfs, 0x18);
    const auto dir_size = valueAt<u64>(romfs, 0x20);
    const auto file_offset = valueAt<u64>(romfs, 0x38);
    const auto file_size = valueAt<u64>(romfs, 0x40);
    const auto content_offset = valueAt<u64>(romfs, 0x48);
    checkRange(dir_offset, dir_size, data.size);
    checkRange(file_offset, file_size, data.size);
    checkRange(content_offset, 0, data.size);
    if (dir_size < 0x18)
        throw std::runtime_error("Truncated Package control directory.");
    const auto root = read(file, data.offset + dir_offset, 0x18);
    const auto files = read(file, data.offset + file_offset, file_size);
    std::set<u32> visited;
    std::vector<Entry> result;
    auto next = valueAt<u32>(root, 0xc);
    while (next != invalid<u32>()) {
        if (!visited.insert(next).second)
            throw std::runtime_error("Cyclic Package control file table.");
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

XciLoader::XciLoader(std::shared_ptr<IFile> program)
    : NcaLoader(program.get()), program_file{std::move(program)} {}

XciLoader* XciLoader::create(std::string_view path,
                             std::filesystem::path content_root) {
    return createPackage(path, false, std::move(content_root));
}

XciLoader* XciLoader::createNsp(std::string_view path,
                                std::filesystem::path content_root) {
    return createPackage(path, true, std::move(content_root));
}

ContentPackage ContentPackage::open(std::string_view path, bool nsp) {
    auto container = std::make_shared<filesystem::DiskFile>(path);
    std::vector<Entry> entries;
    if (nsp) {
        entries = partition(*container, 0, container->getSize(), false);
    } else {
        const auto header = read(*container, 0, 0x200);
        if (valueAt<u32>(header, 0x100) != makeMagic4('H', 'E', 'A', 'D'))
            throw std::runtime_error("Invalid XCI gamecard header.");
        const auto root = valueAt<u64>(header, 0x130);
        checkRange(root, 16, container->getSize());
        const auto partitions =
            partition(*container, root, container->getSize() - root, true);
        const auto secure =
            std::ranges::find(partitions, "secure", &Entry::name);
        if (secure == partitions.end())
            throw std::runtime_error("XCI has no secure game partition.");
        entries = partition(*container, secure->offset, secure->size, true);
    }
    auto keys = NcaFile::loadDefaultKeys();
    {
        for (const auto& entry : entries) {
            if (!std::string_view(entry.name).ends_with(".tik"))
                continue;
            const auto ticket = read(*container, entry.offset, entry.size);
            if (ticket.size() < 4)
                throw std::runtime_error("Truncated NSP ticket.");
            const u32 signature = valueAt<u32>(ticket, 0);
            usize body = 0;
            switch (signature) {
            case 0x010000:
            case 0x010003:
                body = 0x240;
                break;
            case 0x010001:
            case 0x010004:
                body = 0x140;
                break;
            case 0x010002:
            case 0x010005:
                body = 0x80;
                break;
            default:
                throw std::runtime_error(
                    "Unsupported NSP ticket signature type.");
            }
            if (body > ticket.size() || ticket.size() - body < 0x180)
                throw std::runtime_error("Truncated NSP ticket body.");
            if (ticket[body + 0x141] != 0)
                continue; // Personalized title keys need console-specific RSA
                          // keys.
            std::string name = "encrypted_titlekey_";
            for (usize i = 0; i < 16; i++)
                name += fmt::format("{:02x}", ticket[body + 0x160 + i]);
            std::vector<u8> encrypted(
                ticket.begin() + static_cast<std::ptrdiff_t>(body + 0x40),
                ticket.begin() + static_cast<std::ptrdiff_t>(body + 0x50));
            const auto [it, inserted] = keys.emplace(name, encrypted);
            if (!inserted && it->second != encrypted)
                throw std::runtime_error(
                    "NSP contains conflicting tickets for the same content.");
        }
    }
    std::vector<PackageContent> content;
    for (const auto& entry : entries) {
        const std::string_view name(entry.name);
        const bool compressed = name.ends_with(".ncz");
        if (!name.ends_with(".nca") && !compressed)
            continue;
        bool plaintext;
        // NCZ keeps the original NCA header uncompressed.
        auto nca_header = NcaFile::readHeader(*container, entry.offset,
                                              entry.size, keys, plaintext);
        if (!compressed) {
            content.push_back({entry.name, entry.offset, entry.size, container,
                               nca_header, plaintext, false});
            continue;
        }
        const auto nca_size = valueAt<u64>(nca_header, 0x208);
        const auto cache_path =
            std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) /
            "cache/ncz" /
            fmt::format("{}-{:x}.nca", name.substr(0, name.size() - 4),
                        entry.size);
        auto ncz = std::make_shared<filesystem::NczFile>(
            container, entry.offset, entry.size, nca_size, cache_path);
        content.push_back({entry.name, 0, nca_size, std::move(ncz), nca_header,
                           plaintext, true});
    }
    return {std::move(keys), std::move(content)};
}

XciLoader* XciLoader::createPackage(std::string_view path, bool nsp,
                                    std::filesystem::path content_root) {
    auto package = ContentPackage::open(path, nsp);
    const auto& keys = package.keys;
    const auto& content = package.content;
    // Updates may carry the same program ID. Select a base Program NCA, never
    // the largest archive or an update that requires a BKTR overlay.
    const auto bundled_metadata = package.metadata();
    std::set<usize> patch_programs;
    for (const auto& meta : bundled_metadata)
        if (meta.type == 0x81)
            for (const auto& record : meta.records)
                if (record.type == 1)
                    patch_programs.insert(record.content);
    const PackageContent* program = nullptr;
    for (const auto& item : content) {
        if (patch_programs.contains(static_cast<usize>(&item - content.data())))
            continue;
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
            throw std::runtime_error("This Package contains multiple base "
                                     "games. Multi-game selection "
                                     "is not supported yet.");
        program = &item;
    }
    if (program == nullptr)
        throw std::runtime_error(
            "Package has no supported base Program NCA; "
            "use Install Update / DLC for update-only packages.");
    std::shared_ptr<IFile> program_file = program->open(keys);
    auto effective_header = program->header;
    // Metadata identifies the matching update; file size and package order do
    // not.
    auto installed = installedContentPackages(std::move(content_root));
    installed.push_back(package);
    const auto title_id = valueAt<u64>(program->header, 0x210);
    const ContentPackage* update_package = nullptr;
    std::optional<ContentMeta> update;
    std::map<u64, std::pair<usize, ContentMeta>> dlcs;
    for (usize index = 0; index < installed.size(); index++) {
        for (const auto& meta : installed[index].metadata()) {
            if (meta.application_id != title_id)
                continue;
            if (meta.type == 0x81 &&
                (!update || meta.version > update->version)) {
                update = meta;
                update_package = &installed[index];
            } else if (meta.type == 0x82) {
                const auto it = dlcs.find(meta.title_id);
                if (it == dlcs.end() ||
                    meta.version > it->second.second.version)
                    dlcs.insert_or_assign(meta.title_id,
                                          std::make_pair(index, meta));
            }
        }
    }
    if (update) {
        const auto record =
            std::ranges::find_if(update->records, [](const ContentRecord& r) {
                return r.type == 1 && r.id_offset == 0;
            });
        if (record == update->records.end())
            throw std::runtime_error("Update has no main Program NCA.");
        auto patch =
            update_package->content[record->content].open(update_package->keys);
        program_file = filesystem::applyNcaPatch(program_file, patch);
        std::unique_ptr<ztd::io::IStream> header_stream(
            program_file->open(filesystem::FileOpenFlags::Read));
        header_stream->readToSpan(std::span<u8>(effective_header));
        LOG_INFO(Loader, "Applying update {:016x} version {}", update->title_id,
                 update->version);
    }
    // The final virtual NCA must be valid before NcaLoader parses its
    // filesystems. Base validation uses the same checks as updated programs.
    validateProgram(*program_file, effective_header);
    auto loader =
        std::unique_ptr<XciLoader>(new XciLoader(std::move(program_file)));
    loader->content_version = update ? update->version : 0;
    for (const auto& meta : bundled_metadata)
        if (meta.type == 0x80 && meta.application_id == title_id)
            loader->content_version =
                std::max(loader->content_version, meta.version);
    for (const auto& [dlc_id, selected] : dlcs) {
        const auto& [index, meta] = selected;
        if (meta.required_application_version > loader->content_version)
            throw std::runtime_error(fmt::format(
                "DLC {:016x} requires game update version {} or newer.", dlc_id,
                meta.required_application_version));
        const auto record =
            std::ranges::find_if(meta.records, [](const ContentRecord& r) {
                return r.type == 2 && r.id_offset == 0;
            });
        if (record == meta.records.end())
            throw std::runtime_error("DLC has no Data NCA.");
        const auto& item = installed[index].content[record->content];
        auto data = item.open(installed[index].keys);
        const auto region = sectionRegion(item.header, 0, true);
        const auto magic = read(*data, region.offset, 8);
        if (valueAt<u64>(magic, 0) != 0x50)
            throw std::runtime_error("Cannot read DLC RomFS. Check prod.keys.");
        loader->add_on_content.emplace(
            dlc_id, std::make_shared<filesystem::FileView>(data, region.offset,
                                                           region.size));
    }
    // Prefer the selected update's Control NCA (display version and metadata).
    std::vector<std::pair<const PackageContent*, const NcaFile::Keys*>>
        controls;
    if (update)
        for (const auto& record : update->records)
            if (record.type == 3 && record.id_offset == 0)
                controls.emplace_back(&update_package->content[record.content],
                                      &update_package->keys);
    for (const auto& item : content)
        if (item.header[0x205] == 2 &&
            valueAt<u64>(item.header, 0x210) == title_id)
            controls.emplace_back(&item, &keys);
    for (const auto& [ptr, control_keys] : controls) {
        const auto& item = *ptr;

        try {
            auto control = item.open(*control_keys);
            const auto files = controlEntries(*control, item.header);
            for (const auto& file : files) {
                if (file.name == "control.nacp" &&
                    file.size ==
                        sizeof(services::ns::ApplicationControlProperty)) {
                    loader->control_file =
                        std::make_unique<filesystem::MemoryFile>(
                            read(*control, file.offset, file.size));
                    loader->nacp_file = loader->control_file.get();
                } else if (std::string_view(file.name).starts_with("icon_") &&
                           std::string_view(file.name).ends_with(".dat") &&
                           !loader->game_icon) {
                    loader->game_icon =
                        std::make_unique<filesystem::MemoryFile>(
                            read(*control, file.offset, file.size));
                    loader->icon_file = loader->game_icon.get();
                }
            }
            break;
        } catch (const std::exception& error) {
            LOG_WARN(Loader, "Could not load Package control metadata: {}",
                     error.what());
        }
    }
    LOG_INFO(Loader, "Loaded {} base game {:016x} directly from container",
             nsp ? "NSP" : "XCI", loader->getTitleId());
    return loader.release();
}

namespace {
std::array<u8, 32> digest(IFile& file, u64 offset, u64 size) {
    checkRange(offset, size, file.getSize());
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<u8> buffer(1024 * 1024);
    std::unique_ptr<ztd::io::IStream> stream(
        file.open(filesystem::FileOpenFlags::Read));
    stream->seekTo(offset);
    while (size) {
        const auto n = std::min<u64>(size, buffer.size());
        stream->readToSpan(std::span(buffer).first(n));
        CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(n));
        size -= n;
    }
    std::array<u8, 32> result;
    CC_SHA256_Final(result.data(), &context);
    return result;
}
std::filesystem::path contentRoot() {
    return std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) /
           "content/packages";
}
} // namespace

std::shared_ptr<NcaFile> PackageContent::open(const NcaFile::Keys& keys) const {
    return std::make_shared<NcaFile>(source, offset, size, header, plaintext,
                                     keys, compressed);
}

std::vector<ContentMeta> ContentPackage::metadata(bool verify_hashes) const {
    std::vector<ContentMeta> result;
    std::set<u64> ids;
    for (const auto& item : content) {
        if (item.header[0x205] != 1)
            continue;
        auto file = item.open(keys);
        const auto region = sectionRegion(item.header, 0, false);
        const auto entries =
            partition(*file, region.offset, region.size, false);
        for (const auto& entry : entries) {
            if (!std::string_view(entry.name).ends_with(".cnmt"))
                continue;
            const auto bytes = read(*file, entry.offset, entry.size);
            checkRange(0, 0x20, bytes.size());
            const auto type = bytes[0xc];
            if (type != 0x80 && type != 0x81 && type != 0x82)
                throw std::runtime_error("Unsupported content metadata type "
                                         "(delta/data-patch/system package).");
            const auto ext = valueAt<u16>(bytes, 0xe);
            const auto count = valueAt<u16>(bytes, 0x10);
            const auto meta_count = valueAt<u16>(bytes, 0x12);
            if (ext < (type == 0x81 ? 0x18 : 0x10) || count > 4096 ||
                bytes[0xd] != 0 || bytes[0x16] != 0)
                throw std::runtime_error(
                    "Unsupported or incomplete content metadata.");
            checkRange(0x20, ext, bytes.size());
            checkRange(0x20 + ext,
                       u64(count) * 0x38 + u64(meta_count) * 0x10 + 32,
                       bytes.size());
            const u64 extended_data_size =
                type == 0x81 ? valueAt<u32>(bytes, 0x2c) : 0;
            const u64 end = 0x20 + ext + u64(count) * 0x38 +
                            u64(meta_count) * 0x10 + extended_data_size;
            if (end > bytes.size() || bytes.size() - end != 32)
                throw std::runtime_error(
                    "Truncated or oversized content metadata.");
            // The footer is an opaque content-meta digest. Retail CNMT retains
            // the development build's digest, so it is not a checksum of the
            // production CNMT bytes. Referenced NCAs are hashed below.
            ContentMeta meta{valueAt<u64>(bytes, 0),
                             type == 0x80 ? valueAt<u64>(bytes, 0)
                                          : valueAt<u64>(bytes, 0x20),
                             valueAt<u32>(bytes, 8),
                             type == 0x82 ? valueAt<u32>(bytes, 0x28) : 0,
                             type,
                             {}};
            if (valueAt<u64>(item.header, 0x210) != meta.title_id ||
                !ids.insert(meta.title_id).second ||
                (meta.application_id & 0x1fff) != 0 ||
                (type == 0x81 &&
                 meta.title_id != (meta.application_id | 0x800)) ||
                (type == 0x82 &&
                 (meta.title_id < meta.application_id + 0x1000 ||
                  meta.title_id > meta.application_id + 0x1fff)))
                throw std::runtime_error(
                    "Content metadata has invalid or conflicting title IDs.");
            if (type == 0x82 && ext >= 0x18 && valueAt<u64>(bytes, 0x30) != 0)
                throw std::runtime_error(
                    "DLC data-patch dependencies are not supported yet.");
            std::set<std::pair<u8, u8>> record_ids;
            for (u32 j = 0; j < count; j++) {
                const u64 p = 0x20 + ext + j * 0x38;
                std::string id;
                for (usize k = 0; k < 16; k++)
                    id += fmt::format("{:02x}", bytes[p + 0x20 + k]);
                const u8 record_type = bytes[p + 0x36],
                         id_offset = bytes[p + 0x37];
                // Delta fragments are not required to apply a complete patch.
                if (record_type == 6)
                    continue;
                const auto found =
                    std::ranges::find_if(content, [&](const PackageContent& c) {
                        return c.name == id + ".nca" || c.name == id + ".ncz";
                    });
                if (found == content.end())
                    throw std::runtime_error("Package is missing content " +
                                             id + ".nca.");
                u64 expected_size = 0;
                for (usize k = 0; k < 6; k++)
                    expected_size |= u64(bytes[p + 0x30 + k]) << (8 * k);
                constexpr std::array<u8, 7> nca_types{1, 0, 4, 2, 3, 3, 4};
                if (record_type > 5 || found->size != expected_size ||
                    (found->header[0x205] != nca_types[record_type] &&
                     !(record_type == 2 && found->header[0x205] == 5)))
                    throw std::runtime_error(
                        fmt::format("Content {} does not match metadata (size "
                                    "{} vs {}, content type {}, NCA type {}).",
                                    id, found->size, expected_size, record_type,
                                    found->header[0x205]));
                if (!record_ids.emplace(record_type, id_offset).second)
                    throw std::runtime_error(
                        "Duplicate content type/index in metadata.");
                if ((record_type == 1 || record_type == 2 ||
                     record_type == 3) &&
                    valueAt<u64>(found->header, 0x210) !=
                        (type == 0x82 ? meta.title_id : meta.application_id))
                    throw std::runtime_error(
                        "Content NCA belongs to another title.");
                if (record_type == 1 && id_offset != 0)
                    throw std::runtime_error(
                        "Multi-program updates are not supported yet.");
                ContentRecord record{
                    static_cast<usize>(found - content.begin()),
                    record_type,
                    id_offset,
                    {}};
                std::copy_n(bytes.data() + p, 32, record.hash.begin());
                if (verify_hashes) {
                    if (found->compressed)
                        throw std::runtime_error(
                            "Install uncompressed NSP/XCI content; compressed "
                            "update/DLC installation is not supported yet.");
                    if (digest(*found->source, found->offset, found->size) !=
                        record.hash)
                        throw std::runtime_error(
                            "Content SHA-256 does not match its metadata: " +
                            found->name);
                    // Check keys, section ranges and BKTR trees before
                    // installation.
                    auto decoded = found->open(keys);
                    if (record_type == 2) {
                        const auto data = sectionRegion(found->header, 0, true);
                        if (valueAt<u64>(read(*decoded, data.offset, 8), 0) !=
                            0x50)
                            throw std::runtime_error(
                                "Cannot decrypt DLC RomFS. Check prod.keys.");
                    }
                }
                meta.records.push_back(record);
            }
            if (type == 0x81 && !record_ids.contains({1, 0}))
                throw std::runtime_error(
                    "Update is missing its main Program NCA.");
            if (type == 0x82 && !record_ids.contains({2, 0}))
                throw std::runtime_error("DLC is missing its Data NCA.");
            result.push_back(std::move(meta));
        }
    }
    return result;
}

std::string installContentPackage(std::string_view path,
                                  std::filesystem::path root) {
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    if (extension != ".nsp" && extension != ".xci")
        throw std::runtime_error(
            "Choose an update or DLC package in NSP or XCI format.");
    if (root.empty())
        root = contentRoot();
    std::filesystem::create_directories(root);
    // Unique staging file on the same filesystem; only validated packages
    // become visible.
    auto pattern = (root / ".install-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const int fd = mkstemp(name.data());
    if (fd < 0)
        throw std::runtime_error(
            "Cannot create a content installation staging file.");
    close(fd);
    const std::filesystem::path temporary(name.data());
    try {
        std::filesystem::copy_file(
            std::filesystem::path(path), temporary,
            std::filesystem::copy_options::overwrite_existing);
        auto package =
            ContentPackage::open(temporary.string(), extension == ".nsp");
        const auto metadata = package.metadata(true);
        usize updates = 0, dlcs = 0;
        for (const auto& m : metadata) {
            updates += m.type == 0x81;
            dlcs += m.type == 0x82;
        }
        if (!updates && !dlcs)
            throw std::runtime_error("This package contains no installable "
                                     "game update or DLC metadata.");
        filesystem::DiskFile file(temporary.string());
        const auto hash = digest(file, 0, file.getSize());
        std::string id;
        for (const auto byte : hash)
            id += fmt::format("{:02x}", byte);
        const auto destination = root / (id + extension);
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(temporary);
            return "This content is already installed.";
        }
        std::filesystem::rename(temporary, destination);
        return fmt::format("Installed {} game update(s) and {} DLC item(s). "
                           "The newest installed version is selected "
                           "automatically. Restart the game to apply.",
                           updates, dlcs);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}

std::vector<ContentPackage>
installedContentPackages(std::filesystem::path root) {
    std::vector<ContentPackage> result;
    if (root.empty())
        root = contentRoot();
    if (!std::filesystem::exists(root))
        return result;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file() &&
            !entry.path().filename().string().starts_with(".") &&
            (entry.path().extension() == ".nsp" ||
             entry.path().extension() == ".xci"))
            paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        auto package =
            ContentPackage::open(path.string(), path.extension() == ".nsp");
        result.push_back(std::move(package));
    }
    return result;
}

void XciLoader::loadProcess(System& system, kernel::Process* process) {
    NcaLoader::loadProcess(system, process);
    system.getOs().setAddOnContent(add_on_content);
}

} // namespace hydra::horizon::loader

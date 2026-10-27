#include "core/horizon/filesystem/filesystem.hpp"

#include "core/horizon/filesystem/content_archive.hpp"
#include "core/horizon/filesystem/directory.hpp"
#include "core/horizon/filesystem/disk_file.hpp"
#include "core/horizon/filesystem/memory_file.hpp"
#include "core/horizon/filesystem/nca_file.hpp"

#define GET_MOUNT(path)                                                        \
    if (path.empty())                                                          \
        return FsResult::NotMounted;                                           \
    const auto slash_pos = path.find('/');                                     \
    if (slash_pos == std::string::npos)                                        \
        return FsResult::NotMounted;                                           \
    const auto mount = path.substr(0, slash_pos);                              \
    if (mount.empty())                                                         \
        return FsResult::NotMounted;                                           \
    const auto entry_path = path.substr(slash_pos);

#define VERIFY_PATH(path)                                                      \
    GET_MOUNT(path)                                                            \
    auto it = devices.find(std::string(mount));                                \
    if (it == devices.end())                                                   \
        return FsResult::NotMounted;                                           \
    auto& device = it->second;

#define VERIFY_MOUNT(mount)                                                    \
    ASSERT(!mount.contains('/'), Filesystem, "Invalid mount point \"{}\"",     \
           mount);

namespace hydra::horizon::filesystem {

Filesystem::Filesystem() {
    // SD card
    std::filesystem::create_directories(CONFIG_INSTANCE.getSdCardPath());
    mountImpl(FS_SD_MOUNT, new Directory(CONFIG_INSTANCE.getSdCardPath()));

    // Save
    std::filesystem::create_directories(CONFIG_INSTANCE.getSavePath());
    mountImpl(FS_SAVE_MOUNT, new Directory(CONFIG_INSTANCE.getSavePath()));

    // Cache
    // TODO: support mounting to a real host path as well
    mount(FS_CACHE_MOUNT);

    // Content
    mount(FS_CONTENT_MOUNT);

    // Firmware
    installFirmware();
}

void Filesystem::mount(const std::string_view mount) {
    mountImpl(mount, new Directory());
}

void Filesystem::mount(const std::string_view mount,
                       const std::string_view root_path) {
    Directory* root;
    auto res = getDirectory(root_path, root);
    ASSERT(res == FsResult::Success, Filesystem,
           "Failed to get root directory \"{}\" for mount \"{}\"", root_path,
           mount);
    mountImpl(mount, root);
}

FsResult Filesystem::addEntry(const std::string_view path, IEntry* entry,
                              bool add_intermediate) {
    VERIFY_PATH(path);
    return device.addEntry(entry_path, entry, add_intermediate);
}

FsResult Filesystem::addEntry(const std::string_view path,
                              const std::string_view host_path,
                              bool add_intermediate) {
    VERIFY_PATH(path);
    return device.addEntry(entry_path, host_path, add_intermediate);
}

FsResult Filesystem::createFile(const std::string_view path, u64 size,
                                bool add_intermediate) {
    GET_MOUNT(path);

    // TODO: keep a list of host paths for each mount point instead
    if (mount == FS_SD_MOUNT) {
        const auto host_path =
            fmt::format("{}{}", CONFIG_INSTANCE.getSdCardPath(), entry_path);
        auto file = new DiskFile(host_path, true);
        file->resize(size);
        return addEntry(path, file, add_intermediate);
    } else if (mount == FS_SAVE_MOUNT) {
        const auto host_path =
            fmt::format("{}{}", CONFIG_INSTANCE.getSavePath(), entry_path);
        auto file = new DiskFile(host_path, true);
        file->resize(size);
        return addEntry(path, file, add_intermediate);
    } else {
        LOG_WARN(Filesystem,
                 "Could not find host path for path \"{}\", falling back to "
                 "memory backed file",
                 path);
        return addEntry(path, new MemoryFile(size), add_intermediate);
    }
}

FsResult Filesystem::createDirectory(const std::string_view path,
                                     bool add_intermediate) {
    return addEntry(path, new Directory(), add_intermediate);
}

FsResult Filesystem::deleteEntry(const std::string_view path, bool recursive) {
    VERIFY_PATH(path);
    return device.deleteEntry(entry_path, recursive);
}

FsResult Filesystem::getEntry(const std::string_view path, IEntry*& out_entry) {
    VERIFY_PATH(path);
    return device.getEntry(entry_path, out_entry);
}

FsResult Filesystem::getFile(const std::string_view path, IFile*& out_file) {
    VERIFY_PATH(path);
    return device.getFile(entry_path, out_file);
}

FsResult Filesystem::getDirectory(const std::string_view path,
                                  Directory*& out_directory) {
    VERIFY_PATH(path);
    return device.getDirectory(entry_path, out_directory);
}

void Filesystem::mountImpl(const std::string_view mount, Directory* root) {
    VERIFY_MOUNT(mount);
    devices.emplace(mount, root);
    LOG_INFO(Filesystem, "Mounted \"{}\"", mount);
}

void Filesystem::installFirmware() {
    const auto& firmware_path = CONFIG_INSTANCE.getFirmwarePath();
    if (!std::filesystem::exists(firmware_path)) {
        LOG_WARN(Horizon, "Firmware path does not exist");
        // TODO: use replacements
        return;
    }

    std::map<std::string, std::string> firmware_titles_map = {
        {"010000000000080a/data", "AvatarImage"},
        {"010000000000080e/data", "TimeZoneBinary"},
        {"0100000000000810/data", "FontNintendoExtension"},
        {"0100000000000811/data", "FontStandard"},
        {"0100000000000812/data", "FontKorean"},
        {"0100000000000813/data", "FontChineseTraditional"},
        {"0100000000000814/data", "FontChineseSimple"},
    };

    // Only metadata is needed to mount firmware. Parsing every ExeFS/RomFS
    // here repeats work and decrypts unused sections during each startup.
    NcaFile::Keys keys;
    bool keys_loaded = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(firmware_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".nca")
            continue;
        auto file = std::make_unique<DiskFile>(entry.path().string());
        if (file->getSize() < 0xc00)
            throw std::runtime_error("Truncated firmware NCA header.");
        std::unique_ptr<ztd::io::IStream> probe(
            file->open(FileOpenFlags::Read));
        probe->seekTo(0x200);
        if (probe->read<u32>() != makeMagic4('N', 'C', 'A', '3') &&
            !keys_loaded) {
            keys = NcaFile::loadDefaultKeys();
            keys_loaded = true;
        }
        bool plaintext = false;
        const auto header =
            NcaFile::readHeader(*file, 0, file->getSize(), keys, plaintext);
        u64 title_id = 0;
        std::memcpy(&title_id, header.data() + 0x210, sizeof(title_id));
        const auto content_type =
            static_cast<ContentArchiveContentType>(header[0x205]);
        if (header[0x205] >
            static_cast<u8>(ContentArchiveContentType::PublicData))
            throw std::runtime_error("Invalid firmware NCA content type.");
        auto res = addEntry(
            fmt::format(FS_FIRMWARE_PATH "/{:016x}/{}", title_id, content_type),
            file.get(), true);
        if (res != FsResult::Success)
            throw std::runtime_error(fmt::format(
                "Failed to mount firmware title {:016x}: {}", title_id, res));
        file.release();
    }

    for (const auto& [path, filename] : firmware_titles_map) {
        filesystem::IFile* file;
        auto res = getFile(fmt::format(FS_FIRMWARE_PATH "/{}", path), file);
        if (res != FsResult::Success)
            throw std::runtime_error(
                fmt::format("Firmware is incomplete: missing {} ({}). Choose a "
                            "complete firmware folder.",
                            filename, path));

        res =
            addEntry(fmt::format(FS_FIRMWARE_PATH "/{}", filename), file, true);
        if (res != FsResult::Success)
            throw std::runtime_error(fmt::format(
                "Failed to mount firmware resource {}: {}", filename, res));
    }
}

} // namespace hydra::horizon::filesystem

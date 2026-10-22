#pragma once
#include "core/horizon/filesystem/nca_file.hpp"

namespace hydra::horizon::loader {
struct PackageContent {
    std::string name;
    u64 offset, size;
    std::shared_ptr<filesystem::IFile> source;
    filesystem::NcaFile::Header header;
    bool plaintext, compressed;
    std::shared_ptr<filesystem::NcaFile>
    open(const filesystem::NcaFile::Keys& keys) const;
};
struct ContentRecord {
    usize content;
    u8 type, id_offset;
    std::array<u8, 32> hash;
};
struct ContentMeta {
    u64 title_id, application_id;
    u32 version, required_application_version;
    u8 type;
    std::vector<ContentRecord> records;
};
struct ContentPackage {
    filesystem::NcaFile::Keys keys;
    std::vector<PackageContent> content;
    static ContentPackage open(std::string_view path, bool nsp);
    std::vector<ContentMeta> metadata(bool verify_hashes = false) const;
};
// Copies and validates into an immutable local package before publishing it.
std::string installContentPackage(std::string_view path,
                                  std::filesystem::path root = {});
std::vector<ContentPackage>
installedContentPackages(std::filesystem::path root = {});
} // namespace hydra::horizon::loader

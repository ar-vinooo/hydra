#pragma once

#include <memory>
#include <optional>
#include <span>

#include "core/horizon/filesystem/file.hpp"

namespace hydra::horizon::filesystem {

// NCA section compression (CompressionInfo of the FS header, FW 12.0.0+):
// a bucket tree maps virtual ranges to physical blocks that are stored as
// they are, as zeros or LZ4 compressed. The compressed storage sits on the
// data the hash layer covers (IVFC level 6, or the PFS0 region), and the
// filesystem is all of its decompressed storage. Layout from Atmosphere's
// fssystem_compressed_storage.hpp and fssystem_bucket_tree.hpp.
struct CompressionInfo {
    u64 table_offset; // From the start of the hashed data
    u64 table_size;
    u32 entry_count;
};

// The CompressionInfo of an FS header (0x200 bytes), if the section has one
std::optional<CompressionInfo> readCompressionInfo(std::span<const u8> fs_header);

// `data` is the decrypted data of the hash layer, offset 0 being its start.
// Throws on a malformed table.
std::shared_ptr<IFile> openCompressedSection(std::shared_ptr<IFile> data,
                                             const CompressionInfo& info);

// LZ4 block decompression that fails instead of reading or writing out of
// bounds. True if `src` decodes to exactly `dst.size()` bytes.
bool decompressLz4Block(std::span<const u8> src, std::span<u8> dst);

} // namespace hydra::horizon::filesystem

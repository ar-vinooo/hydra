#pragma once

#include "core/horizon/filesystem/nca_file.hpp"

namespace hydra::horizon::filesystem {

// Flat leaf entries from the two-level BKTR tables used by NCA3 patches.
// Layout reference: SciresM/hactool bktr.h (0x4000-byte nodes).
struct BktrEntry {
    u64 offset;
    u64 value;
    u32 source;
};
struct BktrTable {
    u64 end;
    std::vector<BktrEntry> entries;
};
BktrTable readBktrTable(IFile& file, u64 offset, u64 size, u32 entry_count,
                        bool relocation);
std::shared_ptr<IFile> applyNcaPatch(std::shared_ptr<IFile> base,
                                     std::shared_ptr<IFile> patch);

} // namespace hydra::horizon::filesystem

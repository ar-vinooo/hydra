#pragma once

#include <memory>

#include "core/horizon/filesystem/file.hpp"

namespace hydra::horizon::filesystem {

// A read-only view of the NCA stored inside an NCZ (the zstd-compressed NCA
// used by NSZ/XCZ). The first 0x4000 bytes are the original NCA head; the
// remaining bytes are returned decompressed and already decrypted, so the
// result must be opened with NcaFile's decrypted-body mode.
class NczFile final : public IFile {
  public:
    static constexpr u64 HEADER_SIZE = 0x4000;

    // `cache_path` is where a solid (non-block) NCZ is decompressed to, since
    // a single zstd stream cannot be read at random offsets.
    NczFile(std::shared_ptr<IFile> container, u64 offset, u64 size,
            u64 nca_size, std::filesystem::path cache_path);
    ztd::io::IStream* open(FileOpenFlags flags) override;
    u64 getSize() const override;

    bool isBlockCompressed() const;

    struct State;

  private:
    std::shared_ptr<State> state;
};

} // namespace hydra::horizon::filesystem

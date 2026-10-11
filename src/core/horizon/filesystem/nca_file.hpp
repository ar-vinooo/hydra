#pragma once

#include <memory>

#include "core/horizon/filesystem/file.hpp"

namespace hydra::horizon::filesystem {

// A read-only view that decrypts an NCA on demand, without copying the game.
class NcaFile final : public IFile {
  public:
    using Keys = std::map<std::string, std::vector<u8>>;
    using Header = std::array<u8, 0xc00>;

    static Keys loadKeys(const std::filesystem::path& path);
    static Keys loadDefaultKeys();
    static Header readHeader(IFile& file, u64 offset, u64 size,
                             const Keys& keys, bool& plaintext);

    // `decrypted_body` is set for NCZ content, whose sections are stored
    // decrypted, so no title or key-area keys are needed.
    NcaFile(std::shared_ptr<IFile> container, u64 offset, u64 size,
            Header header, bool plaintext, const Keys& keys,
            bool decrypted_body = false);
    ztd::io::IStream* open(FileOpenFlags flags) override;
    u64 getSize() const override;

    struct State;

  private:
    std::shared_ptr<State> state;
};

} // namespace hydra::horizon::filesystem

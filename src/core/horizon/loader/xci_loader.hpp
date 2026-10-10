#pragma once

#include "core/horizon/filesystem/memory_file.hpp"
#include "core/horizon/filesystem/nca_file.hpp"
#include "core/horizon/loader/nca_loader.hpp"

namespace hydra::horizon::loader {

class XciLoader final : public NcaLoader {
  public:
    static XciLoader* create(std::string_view path);

  private:
    explicit XciLoader(std::shared_ptr<filesystem::NcaFile> program);
    std::shared_ptr<filesystem::NcaFile> program_file;
    std::unique_ptr<filesystem::MemoryFile> control_file;
    std::unique_ptr<filesystem::MemoryFile> game_icon;
};

} // namespace hydra::horizon::loader

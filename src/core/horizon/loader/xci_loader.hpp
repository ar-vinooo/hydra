#pragma once

#include "core/horizon/filesystem/memory_file.hpp"
#include "core/horizon/filesystem/nca_file.hpp"
#include "core/horizon/loader/nca_loader.hpp"

namespace hydra::horizon::loader {

class XciLoader final : public NcaLoader {
  public:
    static XciLoader* create(std::string_view path,
                             std::filesystem::path content_root = {});
    static XciLoader* createNsp(std::string_view path,
                                std::filesystem::path content_root = {});
    void loadProcess(System& system, kernel::Process* process) override;
    const auto& getAddOnContent() const { return add_on_content; }
    u32 getContentVersion() const { return content_version; }

  private:
    static XciLoader* createPackage(std::string_view path, bool nsp,
                                    std::filesystem::path content_root);
    explicit XciLoader(std::shared_ptr<filesystem::IFile> program);
    std::shared_ptr<filesystem::IFile> program_file;
    std::map<u64, std::shared_ptr<filesystem::IFile>> add_on_content;
    u32 content_version{};
    std::unique_ptr<filesystem::MemoryFile> control_file;
    std::unique_ptr<filesystem::MemoryFile> game_icon;
};

} // namespace hydra::horizon::loader

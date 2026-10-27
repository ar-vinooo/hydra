#pragma once

#include "core/horizon/loader/loader.hpp"

namespace hydra::horizon::loader {

struct Segment {
    u32 file_offset;
    u32 memory_offset;
    u32 size;
};

class NsoLoader : public ILoader {
  public:
    explicit NsoLoader(filesystem::IFile* file_,
                       const std::string_view name_ = "main",
                       const bool is_entry_point_ = true);

    void setMainThreadParams(u8 priority, u8 core_number, u32 stack_size) {
        main_thread_priority = priority;
        main_thread_core_number = core_number;
        main_thread_stack_size = stack_size;
    }

    void loadProcess(System& system, kernel::Process* process) override;

  private:
    filesystem::IFile* file;
    std::string name;
    const bool is_entry_point;

    u32 text_offset;
    u64 executable_size{0};
    struct {
        Segment seg;
        u64 file_size;
        bool compressed;
    } segments[3];
    u32 dyn_str_offset;
    u32 dyn_str_size;
    u32 dyn_sym_offset;
    u32 dyn_sym_size;

    // TODO: what should the defaults be?
    u8 main_thread_priority{0x2c};
    u8 main_thread_core_number{0};
    u32 main_thread_stack_size{0x40000};
};

// ExeFS code modules in the order the system loader maps them: rtld, main,
// subsdk0-9, sdk, then anything else. rtld resolves symbols in that (address)
// order, so a module that defines its own copy of an sdk symbol must come
// before sdk.
std::vector<std::pair<std::string, filesystem::IFile*>>
getCodeModulesInLoadOrder(filesystem::Directory* exefs_dir);

} // namespace hydra::horizon::loader

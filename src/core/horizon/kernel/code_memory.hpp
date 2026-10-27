#pragma once

#include "core/horizon/kernel/auto_object.hpp"

namespace hydra::horizon::kernel {

class Process;

// Memory of `owner` that is mapped writable elsewhere and executable back in
// the owner (svc::ControlCodeMemory), for JIT compilers
class CodeMemory : public AutoObject {
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::CodeMemory;

    CodeMemory(Process* owner_, vaddr_t addr_, u64 size_,
               std::string_view debug_name = "CodeMemory")
        : AutoObject(TYPE_ID, debug_name), owner{owner_}, addr{addr_},
          size{size_} {}

  private:
    Process* owner;
    vaddr_t addr;
    u64 size;

    // Where the memory is mapped, 0 if it is not
    vaddr_t mapped_addr{0x0};
    vaddr_t owner_mapped_addr{0x0};

  public:
    GETTER(owner, getOwner);
    GETTER(addr, getAddress);
    GETTER(size, getSize);
    GETTER_AND_SETTER(mapped_addr, getMappedAddress, setMappedAddress);
    GETTER_AND_SETTER(owner_mapped_addr, getOwnerMappedAddress,
                      setOwnerMappedAddress);
};

} // namespace hydra::horizon::kernel

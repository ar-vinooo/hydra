#include "core/hw/tegra_x1/cpu/hypervisor/cpu.hpp"

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/os.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/mmu.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/thread.hpp"

namespace hydra::hw::tegra_x1::cpu::hypervisor {

namespace {

constexpr u64 KERNEL_MEM_SIZE = 0x1000;

const u32 exception_handler[] = {
    0xd41fffe2u, // hvc #0xFFFF
    // 0xd69f03e0u, // eret
    // 0xD2B00000, // mov x0, #0x80000000
    // 0xD61F0000, // br  x0
    // Shouldn't happen
    0xd4200000u, // brk #0
};

// Return to the guest. This used to start with 0xd508831f, commented as
// `msr spsel, xzr`, which is TLBI VMALLE1IS: every exception return
// invalidated the TLBs of all cores. Only page table changes need that now.
const u32 exception_trampoline[] = {
    0xd69f03e0u, // eret
    // Shouldn't happen
    0xd4200000u, // brk #0
};

// After this thread relaxed a permission (a write tracking fault): its core
// may still hold the read-only translation
const u32 exception_trampoline_local_tlb[] = {
    0xd508871fu, // tlbi vmalle1
    0xd503379fu, // dsb nsh
    0xd5033fdfu, // isb
    0xd69f03e0u, // eret
    0xd4200000u, // brk #0
};

// After page table changes by any thread (see notePageTableChange)
const u32 exception_trampoline_shared_tlb[] = {
    0xd5033a9fu, // dsb ishst
    0xd508831fu, // tlbi vmalle1is
    0xd5033b9fu, // dsb ish
    0xd5033fdfu, // isb
    0xd69f03e0u, // eret
    0xd4200000u, // brk #0
};

} // namespace

VirtualMachine::VirtualMachine() {
    // Use the available guest physical address width. Host allocations are
    // translated separately and need not fit inside this address space.
    auto config = hv_vm_config_create();
    uint32_t max_bits;
    HV_ASSERT_SUCCESS(hv_vm_config_get_max_ipa_size(&max_bits));
    HV_ASSERT_SUCCESS(hv_vm_config_set_ipa_size(config, max_bits));

    HV_ASSERT_SUCCESS(hv_vm_create(config));
    os_release(config);
}

VirtualMachine::~VirtualMachine() { HV_ASSERT_SUCCESS(hv_vm_destroy()); }

Cpu::Cpu()
    : kernel_mem(align(KERNEL_MEM_SIZE, APPLE_PAGE_SIZE)),
      kernel_page_table(KERNEL_PAGE_TABLE_REGION_BASE) {
    // Create GIC
    // hv_gic_config_t gic_config = hv_gic_config_create();
    // hv_gic_config_set_msi_region_base(&gic_config, TODO);
    // hv_gic_config_set_msi_interrupt_range(&gic_config, TODO);
    // HYP_ASSERT_SUCCESS(hv_gic_create(gic_config));

    // NOTE: this does not really belong to the CPU class, but it's here for
    // simplicity reasons

    // Kernel memory
    kernel_page_table.map(0x0,
                          ztd::Range<uptr>::fromSize(
                              kernel_mem.getPhysicalAddress(), KERNEL_MEM_SIZE),
                          {.type = horizon::kernel::MemoryType::Kernel,
                           .attr = horizon::kernel::MemoryAttribute::None,
                           .perm = horizon::kernel::MemoryPermission::Execute},
                          ApFlags::UserNoneKernelReadExecute);

    for (u64 offset = 0; offset < 0x780; offset += 0x80) {
        memcpy(reinterpret_cast<void*>(kernel_mem.getPtr() + offset),
               exception_handler, sizeof(exception_handler));
    }
    memcpy(reinterpret_cast<void*>(kernel_mem.getPtr() +
                                   EXCEPTION_TRAMPOLINE_OFFSET),
           exception_trampoline, sizeof(exception_trampoline));
    memcpy(reinterpret_cast<void*>(kernel_mem.getPtr() +
                                   EXCEPTION_TRAMPOLINE_LOCAL_TLB_OFFSET),
           exception_trampoline_local_tlb,
           sizeof(exception_trampoline_local_tlb));
    memcpy(reinterpret_cast<void*>(kernel_mem.getPtr() +
                                   EXCEPTION_TRAMPOLINE_SHARED_TLB_OFFSET),
           exception_trampoline_shared_tlb,
           sizeof(exception_trampoline_shared_tlb));

    // Symbols
    // TODO: this should be registered by the process
    /*
    GET_CURRENT_PROCESS_DEBUGGER().GetModuleTable().RegisterSymbol(
        {"Hypervisor::handler",
         ztd::Range<vaddr_t>(KERNEL_REGION_BASE,
                        KERNEL_REGION_BASE + EXCEPTION_TRAMPOLINE_OFFSET)});
    GET_CURRENT_PROCESS_DEBUGGER().GetModuleTable().RegisterSymbol(
        {"Hypervisor::trampoline",
         ztd::Range<vaddr_t>(KERNEL_REGION_BASE + EXCEPTION_TRAMPOLINE_OFFSET,
                        KERNEL_REGION_BASE + EXCEPTION_TRAMPOLINE_OFFSET +
                            sizeof(exception_trampoline))});
    */

    // Features
    features = {.supports_native_breakpoints = true,
                .supports_synchronous_single_step = false};
}

IMmu* Cpu::createMmu(System& system) { return new Mmu(system); }

IThread* Cpu::createThread(WallClock& wall_clock, IMmu* mmu,
                           const ThreadCallbacks& callbacks, IMemory* tls_mem,
                           vaddr_t tls_mem_base) {
    return new Thread(wall_clock, *this, mmu, callbacks, tls_mem, tls_mem_base);
}

IMemory* Cpu::allocateMemory(u64 size) { return new Memory(size); }

IMemory* Cpu::wrapMemory(uptr ptr, u64 size) {
    return new ExternalMemory(ptr, size);
}

} // namespace hydra::hw::tegra_x1::cpu::hypervisor

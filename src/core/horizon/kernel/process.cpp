#include "core/horizon/kernel/process.hpp"

#include <random>
#include <sys/mman.h>
#include <unistd.h>

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/guest_thread.hpp"
#include "core/horizon/kernel/shared_memory.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"
#include "core/system.hpp"

namespace hydra::horizon::kernel {

Process::Process(System& system_, std::string_view debug_name)
    : SynchronizationObject(TYPE_ID, false, debug_name), system{system_},
      mmu{system.getCpu().createMmu(system)}, gmmu(mmu.get()),
      applet_state(system.getOs().getKernel()) {
    // TODO: use title ID and name as debugger name?
    DEBUGGER_MANAGER_INSTANCE.attachDebugger(
        this,
        /*fmt::format("{:016x}", title_id)*/ getDebugName());

    // Random entropy
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (auto& random_e : random_entropy)
        random_e = gen();
}

Process::~Process() {
    cleanUp();
    DEBUGGER_MANAGER_INSTANCE.detachDebugger(this);
}

uptr Process::createMemory(ztd::Range<vaddr_t> region, u64 size,
                           MemoryType type, MemoryPermission perm,
                           vaddr_t& out_base) {
    out_base = mmu->findFreeMemory(region, size);
    ASSERT(out_base != 0x0, Kernel, "Failed to find free memory");

    auto mem = system.getCpu().allocateMemory(size);
    mmu->map(out_base, mem,
             {.type = type, .attr = MemoryAttribute::None, .perm = perm});
    executable_mems.emplace_back(mem);

    return mem->getPtr();
}

uptr Process::createExecutableMemory(const std::string_view module_name,
                                     CodeSet code_set, vaddr_t& out_base) {
    // TODO: use MemoryType::Static?
    auto ptr = createMemory(EXECUTABLE_REGION, code_set.size,
                            static_cast<MemoryType>(3), MemoryPermission::Read,
                            out_base);

    // Protect
    mmu->protect(
        ztd::Range<vaddr_t>::fromSize(
            out_base + code_set.code.getBegin(),
            align(code_set.code.getSize(), hw::tegra_x1::cpu::GUEST_PAGE_SIZE)),
        MemoryPermission::ReadExecute);
    // mmu->Protect(
    //     ztd::Range<vaddr_t>::fromSize(out_base + code_set.ro_data.getBegin(),
    //                              align(code_set.ro_data.GetSize(),
    //                                    hw::tegra_x1::cpu::GUEST_PAGE_SIZE)),
    //     MemoryPermission::Read);
    mmu->protect(
        ztd::Range<vaddr_t>::fromSize(
            out_base + code_set.data.getBegin(),
            align(code_set.data.getSize(), hw::tegra_x1::cpu::GUEST_PAGE_SIZE)),
        MemoryPermission::ReadWrite);

    // Debug
    DEBUGGER_MANAGER_INSTANCE.getDebugger(this).getModuleTable().registerSymbol(
        {.name = std::string(module_name),
         .guest_mem_range =
             ztd::Range<vaddr_t>(out_base, out_base + code_set.size)});

    return ptr;
}

hw::tegra_x1::cpu::IMemory* Process::createTlsMemory(vaddr_t& base) {
    auto mem = system.getCpu().allocateMemory(TLS_SIZE);
    base = tls_mem_base;
    mmu->map(base, mem,
             {.type = MemoryType::ThreadLocal,
              .attr = MemoryAttribute::None,
              .perm = MemoryPermission::ReadWrite});
    tls_mem_base += TLS_SIZE;

    return mem;
}

void Process::createStackMemory(u64 stack_size) {
    // main_thread = new GuestThread(this, STACK_REGION.begin + stack_size -
    // 0x10, priority); auto handle = AddHandle(main_thread);

    main_thread_stack_mem.reset(system.getCpu().allocateMemory(stack_size));
    mmu->map(STACK_REGION.getBegin(), main_thread_stack_mem.get(),
             {.type = MemoryType::Stack,
              .attr = MemoryAttribute::None,
              .perm = MemoryPermission::ReadWrite});
}

u64 Process::getUsedUserMemorySize() const {
    u64 used = tls_mem_base - TLS_REGION.getBegin();
    for (const auto& memory : executable_mems)
        used += memory->getSize();
    if (main_thread_stack_mem)
        used += main_thread_stack_mem->getSize();
    {
        std::scoped_lock lock(heap_mutex);
        used += heap_size;
    }
    {
        std::scoped_lock lock(physical_mem_mutex);
        used += physical_mem_size;
    }
    return used;
}

void Process::resizeHeap(u64 size) {
    // The heap lives in one lazily committed host reservation and is resized in
    // place. Its host pointer never changes, so there is no copy that can lose
    // concurrent guest or GPU writes, and GPU mappings and pointers already
    // encoded in Metal stay valid.
    std::scoped_lock lock(heap_mutex);
    if (heap_host_base == 0x0) {
        void* ptr = mmap(nullptr, HEAP_REGION.getSize(), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        ASSERT(ptr != MAP_FAILED, Kernel, "Failed to reserve heap memory");
        heap_host_base = reinterpret_cast<uptr>(ptr);
    }
    ASSERT(size <= HEAP_REGION.getSize(), Kernel, "Heap size {:#x} too big", size);

    const u64 old_size = heap_size;
    if (size > old_size) {
        // Grown memory reads as zero, even if the pages were used before.
        // Only clear pages that were used: clearing untouched ones would
        // commit the whole new heap at once
        const u64 dirty_end = std::min(size, heap_dirty_size);
        if (dirty_end > old_size)
            std::memset(reinterpret_cast<void*>(heap_host_base + old_size), 0,
                        dirty_end - old_size);
        heap_dirty_size = std::max(heap_dirty_size, size);

        // A guest mapping may not cross a slab, as each slab is a separate CPU
        // memory object
        for (u64 offset = old_size; offset < size;) {
            const u64 slab_index = offset / HEAP_SLAB_SIZE;
            const u64 slab_begin = slab_index * HEAP_SLAB_SIZE;
            auto& slab = heap_slabs[slab_index];
            if (slab == nullptr)
                slab.reset(system.getCpu().wrapMemory(heap_host_base + slab_begin,
                                                      HEAP_SLAB_SIZE));
            const u64 run_end = std::min(size, slab_begin + HEAP_SLAB_SIZE);
            mmu->map(HEAP_REGION.getBegin() + offset,
                     ztd::Range<uptr>(heap_host_base + offset,
                                      heap_host_base + run_end),
                     {.type = MemoryType::Normal_1_0_0,
                      .attr = MemoryAttribute::None,
                      .perm = MemoryPermission::ReadWrite});
            offset = run_end;
        }
        heap_size = size;
    } else if (size < old_size) {
        const uptr tail_begin = heap_host_base + size;
        const uptr tail_end = heap_host_base + old_size;
        const auto tail = ztd::Range<uptr>(tail_begin, tail_end);
        system.getGpu().getRenderer().flushMemoryForCpu(tail);
        mmu->unmap(ztd::Range<vaddr_t>::fromSize(HEAP_REGION.getBegin() + size,
                                                 old_size - size));
        heap_size = size;

        // GPU mappings beyond the new end are dropped; the rest stay in place
        std::vector<std::shared_ptr<hw::tegra_x1::gpu::GMmu>> spaces;
        {
            std::scoped_lock gpu_lock(gpu_spaces_mutex);
            for (const auto& weak : gpu_spaces)
                if (auto space = weak.lock())
                    spaces.push_back(std::move(space));
        }
        const auto drop_tail = [&](hw::tegra_x1::gpu::GMmu& space) {
            space.rebaseHostRange(tail_begin, tail_end, tail_begin, 0);
        };
        for (const auto& space : spaces)
            drop_tail(*space);
        drop_tail(gmmu);
        system.getGpu().getRenderer().invalidateMemory(tail);

        // Give the pages back to the host; they are zeroed when grown again
        const uptr host_begin =
            align(tail_begin, static_cast<uptr>(getpagesize()));
        const uptr host_end =
            alignDown(tail_end, static_cast<uptr>(getpagesize()));
        if (host_begin < host_end)
            madvise(reinterpret_cast<void*>(host_begin), host_end - host_begin,
                    MADV_FREE_REUSABLE);
    }
}

void Process::mapAliasRun(vaddr_t begin, vaddr_t end) {
    if (begin == end)
        return;
    // DEBUG: random data aborts in freshly mapped alias memory
    if (std::getenv("HYDRA_TRACE_SVC") != nullptr)
        LOG_INFO(Kernel, "DEBUG alias map run {:#x}..{:#x}", begin, end);

    if (alias_host_base == 0x0) {
        void* ptr = mmap(nullptr, ALIAS_REGION.getSize(), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        ASSERT(ptr != MAP_FAILED, Kernel, "Failed to reserve alias memory");
        alias_host_base = reinterpret_cast<uptr>(ptr);
    }

    // New mappings must read as zero, even if the pages were used before
    std::memset(reinterpret_cast<void*>(getAliasHostPtr(begin)), 0,
                end - begin);

    // A guest mapping may not cross a slab, as each slab is a separate CPU
    // memory object
    for (vaddr_t va = begin; va < end;) {
        const u64 slab_index = (va - ALIAS_REGION.getBegin()) / ALIAS_SLAB_SIZE;
        const vaddr_t slab_begin =
            ALIAS_REGION.getBegin() + slab_index * ALIAS_SLAB_SIZE;
        auto& slab = alias_slabs[slab_index];
        if (slab == nullptr)
            slab.reset(system.getCpu().wrapMemory(getAliasHostPtr(slab_begin),
                                                  ALIAS_SLAB_SIZE));

        const vaddr_t run_end = std::min(end, slab_begin + ALIAS_SLAB_SIZE);
        mmu->map(va,
                 ztd::Range<uptr>(getAliasHostPtr(va), getAliasHostPtr(run_end)),
                 {.type = MemoryType::Alias,
                  .attr = MemoryAttribute::None,
                  .perm = MemoryPermission::ReadWrite});
        va = run_end;
    }

    physical_mem_size += end - begin;
}

void Process::releaseHeapMemory() {
    std::scoped_lock lock(heap_mutex);
    heap_slabs.clear();
    if (heap_host_base != 0x0) {
        munmap(reinterpret_cast<void*>(heap_host_base), HEAP_REGION.getSize());
        heap_host_base = 0x0;
    }
    heap_size = 0;
    heap_dirty_size = 0;
}

void Process::releaseAliasMemory() {
    std::scoped_lock lock(physical_mem_mutex);
    alias_slabs.clear();
    if (alias_host_base != 0x0) {
        munmap(reinterpret_cast<void*>(alias_host_base), ALIAS_REGION.getSize());
        alias_host_base = 0x0;
    }
    physical_mem_size = 0;
}

result_t Process::mapSharedMemory(SharedMemory* memory, vaddr_t addr, u64 size,
                                  MemoryPermission perm) {
    if (!memory)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    if (size == 0 || size != memory->getSize())
        return MAKE_RESULT(Svc, Error::InvalidSize);
    if (addr % hw::tegra_x1::cpu::GUEST_PAGE_SIZE != 0 ||
        addr >= ADDRESS_SPACE.getEnd() || size > ADDRESS_SPACE.getEnd() - addr)
        return MAKE_RESULT(Svc, Error::InvalidAddress);
    std::scoped_lock lock(shared_memory_mutex);
    if (shared_memory_stopped)
        return MAKE_RESULT(Svc, Error::InvalidState);
    for (u64 va = addr; va < addr + size;) {
        const auto region = mmu->queryRegion(va);
        if (region.state.type != MemoryType::Free)
            return MAKE_RESULT(Svc, Error::InvalidMemoryState);
        const auto step = std::min(size - (va - addr), region.size - (va - region.va));
        if (step == 0)
            return MAKE_RESULT(Svc, Error::InvalidMemoryRange);
        va += step;
    }
    try {
        shared_backings.try_emplace(memory->getPtr(), memory->memory);
        const auto [mapping, inserted] = shared_mappings.emplace(addr, SharedMapping{memory, size});
        if (!inserted)
            return MAKE_RESULT(Svc, Error::InvalidMemoryState);
        memory->retain();
        try {
            memory->mapToRange(mmu.get(), ztd::Range<vaddr_t>::fromSize(addr, size), perm);
        } catch (...) {
            shared_mappings.erase(mapping);
            memory->release();
            throw;
        }
    } catch (const std::bad_alloc&) {
        return MAKE_RESULT(Svc, Error::OutOfMemory);
    }
    return RESULT_SUCCESS;
}

result_t Process::unmapSharedMemory(SharedMemory* memory, vaddr_t addr, u64 size) {
    std::scoped_lock lock(shared_memory_mutex);
    if (shared_memory_stopped)
        return MAKE_RESULT(Svc, Error::InvalidState);
    const auto mapping = shared_mappings.find(addr);
    if (mapping == shared_mappings.end() || mapping->second.object != memory)
        return MAKE_RESULT(Svc, Error::InvalidMemoryState);
    if (size != mapping->second.size)
        return MAKE_RESULT(Svc, Error::InvalidSize);
    mmu->unmap(ztd::Range<vaddr_t>::fromSize(addr, size));
    shared_mappings.erase(mapping);
    memory->release();
    return RESULT_SUCCESS;
}

result_t Process::mapPhysicalMemory(vaddr_t addr, u64 size) {
    std::scoped_lock lock(physical_mem_mutex);
    if (std::getenv("HYDRA_TRACE_SVC") != nullptr)
        LOG_INFO(Kernel, "DEBUG map physical {:#x}..{:#x}", addr, addr + size);

    // Only the unmapped parts of the range get new memory
    const auto range = ztd::Range<vaddr_t>::fromSize(addr, size);
    vaddr_t run_begin = range.getBegin();
    for (vaddr_t va = range.getBegin(); va < range.getEnd();) {
        const auto region = mmu->queryRegion(va);
        const auto region_end = std::min<vaddr_t>(
            std::max<vaddr_t>(region.va + region.size,
                              va + hw::tegra_x1::cpu::GUEST_PAGE_SIZE),
            range.getEnd());
        if (region.state.type != MemoryType::Free) {
            mapAliasRun(run_begin, va);
            run_begin = region_end;
        }
        va = region_end;
    }
    mapAliasRun(run_begin, range.getEnd());

    return RESULT_SUCCESS;
}

result_t Process::unmapPhysicalMemory(vaddr_t addr, u64 size) {
    std::scoped_lock lock(physical_mem_mutex);
    if (std::getenv("HYDRA_TRACE_SVC") != nullptr)
        LOG_INFO(Kernel, "DEBUG unmap physical {:#x}..{:#x}", addr, addr + size);

    const auto range = ztd::Range<vaddr_t>::fromSize(addr, size);

    // Every mapped page in the range must come from MapPhysicalMemory
    u64 unmapped_size = 0;
    for (vaddr_t va = range.getBegin(); va < range.getEnd();
         va += hw::tegra_x1::cpu::GUEST_PAGE_SIZE) {
        const auto type = mmu->queryRegion(va).state.type;
        if (type == MemoryType::Alias)
            unmapped_size += hw::tegra_x1::cpu::GUEST_PAGE_SIZE;
        else if (type != MemoryType::Free)
            return MAKE_RESULT(Svc, Error::InvalidMemoryState);
    }
    if (unmapped_size == 0)
        return RESULT_SUCCESS;

    mmu->unmap(range);
    physical_mem_size -= unmapped_size;

    // The host pages are reused when the range is mapped again, which neither
    // goes through the guest nor keeps its write tracking: drop cached copies
    system.getGpu().getRenderer().invalidateMemory(ztd::Range<uptr>(
        getAliasHostPtr(range.getBegin()), getAliasHostPtr(range.getEnd())));

    // Give the pages back to the host; they are zeroed when mapped again
    const uptr host_begin = align(getAliasHostPtr(range.getBegin()),
                                  static_cast<uptr>(getpagesize()));
    const uptr host_end = alignDown(getAliasHostPtr(range.getEnd()),
                                    static_cast<uptr>(getpagesize()));
    if (host_begin < host_end)
        madvise(reinterpret_cast<void*>(host_begin), host_end - host_begin,
                MADV_FREE_REUSABLE);

    return RESULT_SUCCESS;
}

void Process::start() {
    // Main thread
    main_thread->start();

    // Signal
    signalStateChange(ProcessState::Started);
}

void Process::stop() {
    std::scoped_lock lock(thread_mutex);
    stopping = true;
    for (auto thread : threads)
        thread->stop();

    // Signal
    signalStateChange(ProcessState::Exiting);
}

void Process::joinThreads() {
    stop();
    // A running thread can start another thread while the first snapshot joins.
    do {
        std::vector<IThread*> pending;
        {
            std::scoped_lock lock(thread_mutex);
            pending = threads;
            if (main_thread && std::find(pending.begin(), pending.end(),
                                         main_thread) == pending.end())
                pending.push_back(main_thread);
            for (auto thread : pending)
                thread->retain();
        }
        // Joining under thread_mutex would deadlock unregisterThread().
        for (auto thread : pending)
            thread->stopAndJoin();
        for (auto thread : pending)
            thread->release();
    } while (isRunning());
    // Also join threads that unregistered just before the snapshot; their final
    // signal/debugger callbacks must finish before kernel locks are destroyed.
    for (auto object : handle_pool)
        if (object->isOfType<IThread>())
            static_cast<IThread*>(object)->stopAndJoin();
}

void Process::supervisorPause() {
    std::scoped_lock lock(thread_mutex);
    for (auto thread : threads)
        thread->supervisorPause();

    // Signal
    signalStateChange(ProcessState::DebugSuspended);
}

void Process::supervisorResume() {
    std::scoped_lock lock(thread_mutex);
    for (auto thread : threads)
        thread->supervisorResume();

    // Signal
    signalStateChange(ProcessState::Started);
}

std::shared_ptr<hw::tegra_x1::gpu::GMmu> Process::createGpuAddressSpace() {
    std::scoped_lock lock(gpu_spaces_mutex);
    if (gpu_spaces_stopped)
        return {};
    auto space = std::make_shared<hw::tegra_x1::gpu::GMmu>(mmu.get());
    std::erase_if(gpu_spaces, [](const auto& weak) { return weak.expired(); });
    gpu_spaces.push_back(space);
    return space;
}

void Process::cleanUp() {
    joinThreads();
    {
        std::scoped_lock lock(shared_memory_mutex);
        shared_memory_stopped = true;
    }
    std::vector<std::shared_ptr<hw::tegra_x1::gpu::GMmu>> spaces;
    {
        std::scoped_lock lock(gpu_spaces_mutex);
        gpu_spaces_stopped = true;
        for (const auto& weak : gpu_spaces) {
            if (auto space = weak.lock()) {
                space->cancelGpuWork();
                spaces.push_back(std::move(space));
            }
        }
        gpu_spaces.clear();
    }
    // Cancel all spaces before draining: channels in different spaces may wait
    // on each other. Guest backing remains alive until every drain completes.
    gmmu.cancelGpuWork();
    for (const auto& space : spaces)
        system.getGpu().getPfifo().cancel(*space);
    system.getGpu().getPfifo().cancel(gmmu);
    {
        std::scoped_lock lock(shared_memory_mutex);
        for (const auto& [addr, mapping] : shared_mappings)
            mapping.object->release();
        shared_mappings.clear();
        shared_backings.clear();
    }
    executable_mems.clear();
    main_thread_stack_mem = nullptr;
    releaseHeapMemory();
    releaseAliasMemory();

    // Main thread
    if (main_thread != nullptr) {
        main_thread->release();
        main_thread = nullptr;
    }

    for (const auto& obj : handle_pool) {
        obj->release();
    }

    // Signal
    signalStateChange(ProcessState::Exited);
}

void Process::signalStateChange(ProcessState new_state) {
    state = new_state;
    signal();
}

void Process::debugDumpThreads() {
    std::scoped_lock lock(thread_mutex);
    LOG_INFO(Kernel, "THREADDUMP begin ({} threads)", threads.size());
    for (auto* thread : threads) {
        // No RTTI; guest threads are named "Guest thread ..."
        if (!thread->getDebugName().starts_with("Guest thread"))
            continue;
        auto* cpu_thread = static_cast<GuestThread*>(thread)->getThread();
        if (cpu_thread == nullptr)
            continue;
        const auto& cpu_state = cpu_thread->getState();
        std::string frames;
        vaddr_t fp = cpu_state.fp;
        for (u32 i = 0; i < 12 && fp != 0; i++) {
            u64 next_fp = 0, ret = 0;
            if (!mmu->tryRead(fp, next_fp) || !mmu->tryRead(fp + 8, ret))
                break;
            frames += fmt::format(" {:#x}", ret);
            fp = next_fp;
        }
        LOG_INFO(Kernel, "THREADDUMP {} pc={:#x} lr={:#x} x0={:#x} x1={:#x} "
                         "frames:{}",
                 thread->getDebugName(), cpu_state.pc, cpu_state.lr, cpu_state.r[0],
                 cpu_state.r[1], frames);
        // DEBUG: memory around the waited mutex (nn::os EventType state)
        std::string words;
        for (u64 off = 0; cpu_state.r[0] >= 0x10000000 && off < 0x30; off += 4) {
            u32 word = 0;
            mmu->tryRead(cpu_state.r[0] - 0x20 + off, word);
            words += fmt::format(" {:08x}", word);
        }
        LOG_INFO(Kernel, "THREADDUMP   mem[x0-0x20]:{}", words);
    }
    LOG_INFO(Kernel, "THREADDUMP end");
}

} // namespace hydra::horizon::kernel

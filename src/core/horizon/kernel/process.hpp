#pragma once

#include <condition_variable>

#include "core/horizon/handle_pool.hpp"
#include "core/horizon/kernel/applet_state.hpp"
#include "core/horizon/kernel/synchronization_object.hpp"
#include "core/horizon/kernel/resource_limit.hpp"
#include "core/horizon/kernel/thread.hpp"
#include "core/hw/tegra_x1/cpu/memory.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"

// TODO: remove dependency
#include "core/horizon/kernel/guest_thread.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
} // namespace hydra::hw::tegra_x1::cpu

namespace hydra::horizon::kernel {

class SharedMemory;

enum class ProcessState {
    Created = 0,
    CreatedAttached = 1,
    Started = 2,
    Crashed = 3, // Only in debug mode
    StartedAttached = 4,
    Exiting = 5,
    Exited = 6,
    DebugSuspended = 7,
};

struct CodeSet {
    u64 size;
    ztd::Range<u64> code;
    ztd::Range<u64> ro_data;
    ztd::Range<u64> data;
};

} // namespace hydra::horizon::kernel

namespace hydra::horizon::services::ns {
struct ApplicationControlProperty;
}

namespace hydra::horizon::kernel {

class Process : public SynchronizationObject {
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::Process;

    explicit Process(System& system_, std::string_view debug_name = "Process");
    ~Process() override;

    // Memory
    uptr createMemory(ztd::Range<vaddr_t> region, u64 size, MemoryType type,
                      MemoryPermission perm, vaddr_t& out_base);
    uptr createExecutableMemory(const std::string_view module_name,
                                CodeSet code_set, vaddr_t& out_base);
    hw::tegra_x1::cpu::IMemory* createTlsMemory(vaddr_t& base);
    void createStackMemory(u64 stack_size);
    void resizeHeap(u64 size);
    result_t mapPhysicalMemory(vaddr_t addr, u64 size);
    result_t unmapPhysicalMemory(vaddr_t addr, u64 size);
    result_t mapSharedMemory(SharedMemory* memory, vaddr_t addr, u64 size,
                             MemoryPermission perm);
    result_t unmapSharedMemory(SharedMemory* memory, vaddr_t addr, u64 size);

    // Thread
    Handle setMainThread(GuestThread* thread) {
        main_thread = thread;
        return addHandle(main_thread);
    }

    void registerThread(IThread* thread) {
        std::scoped_lock lock(thread_mutex);
        threads.push_back(thread);
        resource_limit->add(LimitableResource::ThreadCountMax);
        if (stopping)
            thread->stop();
    }
    // DEBUG: log PC/LR and the frame-pointer chain of every guest thread
    void debugDumpThreads();

    void unregisterThread(IThread* thread) {
        std::scoped_lock lock(thread_mutex);
        if (std::erase(threads, thread) != 0)
            resource_limit->remove(LimitableResource::ThreadCountMax);

        // Signal
        if (threads.empty())
            signalStateChange(ProcessState::Exited);
    }

    void start();
    void stop();
    void joinThreads();

    void supervisorPause();
    void supervisorResume();

    bool isRunning() {
        std::scoped_lock lock(thread_mutex);
        return !threads.empty();
    }

    ProcessState getState() const { return state; }

    // Helpers

    // Handles
    template <typename T>
    // TODO: uncomment
    /*std::optional<T*>*/ T* getHandle(Handle handle) {
        static_assert(std::is_base_of_v<AutoObject, T>,
                      "T must be derived from AutoObject");

        if (!handle.isValid())
            return nullptr; // TODO: std::nullopt

        if constexpr (std::is_base_of_v<T, Process>) {
            if (handle == CURRENT_PROCESS_PSEUDO_HANDLE) [[unlikely]] {
                return this;
            }
        }

        if constexpr (std::is_base_of_v<T, IThread>) {
            if (handle == CURRENT_THREAD_PSEUDO_HANDLE) [[unlikely]] {
                return tls_current_thread;
            }
        }

        // Guest-provided handles may be garbage; the pool is not bounds checked
        if (handle.getRaw() > HANDLE_POOL_CAPACITY) [[unlikely]] {
            LOG_WARN(Kernel, "Invalid handle 0x{:x}", handle.getRaw());
            return nullptr;
        }

        // A handle of another type is invalid, as on Horizon, instead of
        // being reinterpreted as T
        auto obj = handle_pool.get(handle).value_or(nullptr);
        if (obj == nullptr)
            return nullptr;
        auto typed = obj->template as<T>();
        if (typed == nullptr) [[unlikely]]
            LOG_WARN(Kernel, "Handle 0x{:x} is a {}, not the expected type",
                     handle.getRaw(), obj->getTypeId());
        return typed;
    }

    Handle addHandleNoRetain(AutoObject* obj) {
        // TODO: remove
        if (obj == nullptr) [[unlikely]]
            return INVALID_HANDLE;

        return handle_pool.insert(obj).value();
    }

    Handle addHandle(AutoObject* obj) {
        // TODO: remove
        if (obj == nullptr) [[unlikely]]
            return INVALID_HANDLE;

        obj->retain();
        return handle_pool.insert(obj).value();
    }

    bool freeHandle(Handle handle) {
        ASSERT_DEBUG(handle != CURRENT_PROCESS_PSEUDO_HANDLE, Kernel,
                     "Cannot free current process handle");
        ASSERT_DEBUG(handle != CURRENT_THREAD_PSEUDO_HANDLE, Kernel,
                     "Cannot free current thread handle");

        if (handle.getRaw() > HANDLE_POOL_CAPACITY) [[unlikely]] {
            LOG_WARN(Kernel, "Invalid handle 0x{:x}", handle.getRaw());
            return false;
        }

        const auto object = handle_pool.get(handle);
        if (!object.has_value()) {
            LOG_WARN(Kernel, "Invalid handle {}", handle);
            return false;
        }

        object.value()->release();
        ASSERT_DEBUG(handle_pool.free(handle), Kernel,
                     "Failed to free handle {}", handle);
        return true;
    }

    hw::tegra_x1::cpu::IMmu* getMmu() const { return mmu.get(); }
    uptr getHeapHostPtr() const { return heap_host_base; }
    u64 getHeapSize() const {
        std::scoped_lock lock(heap_mutex);
        return heap_size;
    }
    u64 getUsedUserMemorySize() const;
    std::shared_ptr<hw::tegra_x1::gpu::GMmu> createGpuAddressSpace();

  private:
    System& system;

    std::unique_ptr<hw::tegra_x1::cpu::IMmu> mmu;
    hw::tegra_x1::gpu::GMmu gmmu;
    std::mutex gpu_spaces_mutex;
    std::vector<std::weak_ptr<hw::tegra_x1::gpu::GMmu>> gpu_spaces;
    bool gpu_spaces_stopped{false};

    AppletState applet_state;

    u64 title_id{invalid<u64>()};
    u32 system_resource_size{invalid<u32>()};

    // Random entropy
    std::array<u64, 4> random_entropy;

    // Memory
    std::vector<std::unique_ptr<hw::tegra_x1::cpu::IMemory>> executable_mems;
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> main_thread_stack_mem;
    // The heap is one lazily committed host reservation that is resized in
    // place (see resizeHeap), exposed to the CPU in slabs on first use.
    static constexpr u64 HEAP_SLAB_SIZE = 32ull * 1024 * 1024;
    mutable std::mutex heap_mutex;
    uptr heap_host_base{0};
    u64 heap_size{0};
    // Highest heap size ever used: host pages past it were never touched and
    // still read as zero
    u64 heap_dirty_size{0};
    std::map<u64, std::unique_ptr<hw::tegra_x1::cpu::IMemory>> heap_slabs;
    void releaseHeapMemory();
    std::mutex shared_memory_mutex;
    struct SharedMapping {
        SharedMemory* object;
        u64 size;
    };
    std::map<vaddr_t, SharedMapping> shared_mappings;
    // CPU unmap does not retire GPU mappings or pointers encoded in Metal.
    std::map<uptr, std::shared_ptr<hw::tegra_x1::cpu::IMemory>> shared_backings;
    bool shared_memory_stopped{false};

    // Memory mapped with svcMapPhysicalMemory. The whole alias region is
    // backed by one lazily committed host reservation, so memory that is
    // contiguous for the guest is also contiguous for the host (IPC buffers
    // rely on this). It is exposed to the CPU in slabs on first use.
    static constexpr u64 ALIAS_SLAB_SIZE = 32ull * 1024 * 1024;
    mutable std::mutex physical_mem_mutex;
    uptr alias_host_base{0};
    std::map<u64, std::unique_ptr<hw::tegra_x1::cpu::IMemory>> alias_slabs;
    u64 physical_mem_size{0};

    uptr getAliasHostPtr(vaddr_t va) const {
        return alias_host_base + (va - ALIAS_REGION.getBegin());
    }
    void mapAliasRun(vaddr_t begin, vaddr_t end);
    void releaseAliasMemory();

    vaddr_t tls_mem_base{TLS_REGION.getBegin()};

    // Horizon gives IDs up to 0x50 to the initial kernel processes; the
    // first application gets 0x51
    static inline std::atomic<u64> next_process_id{0x51};
    const u64 process_id{next_process_id.fetch_add(1, std::memory_order_relaxed)};

    std::unique_ptr<services::ns::ApplicationControlProperty> control_property;

    ResourceLimit* resource_limit;

    // User exception handling
    vaddr_t entry_point{0x0};
    std::mutex exception_mutex;
    std::condition_variable exception_cv;
    IThread* exception_thread{nullptr};
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> local_region_mem;
    vaddr_t local_region{0x0};

    // Thread
    GuestThread* main_thread{nullptr};
    // From the ThreadInfo kernel capability and the NPDM: the cores and
    // priorities this process may use, and the ideal core of its threads.
    // Defaults allow everything, for processes without capabilities.
    u64 core_mask{0xf};
    i32 highest_thread_priority{0};
    i32 lowest_thread_priority{63};
    i32 ideal_core{0};
    std::mutex thread_mutex;
    std::vector<IThread*> threads;
    bool stopping{false};

    // Handles
    // TODO: store as strong refs
    static constexpr usize HANDLE_POOL_CAPACITY = 512;
    StaticHandlePool<AutoObject*, HANDLE_POOL_CAPACITY>
        handle_pool; // TODO: get the size from capabilities

    std::atomic<ProcessState> state{ProcessState::Created};

    void cleanUp();

    void signalStateChange(ProcessState new_state);

  public:
    REF_GETTER(gmmu, getGMmu);
    REF_GETTER(applet_state, getAppletState);
    GETTER_AND_SETTER(title_id, getTitleId, setTitleId);
    GETTER_AND_SETTER(system_resource_size, getSystemResourceSize,
                      setSystemResourceSize);
    CONST_REF_GETTER(random_entropy, getRandomEntropy);
    GETTER(main_thread, getMainThread);
    u64 getProcessId() const { return process_id; }
    GETTER_AND_SETTER(core_mask, getCoreMask, setCoreMask);
    // The limits pm gives applications, with the current memory use
    ResourceLimit* getResourceLimit();

    // User exception handling (svc::ReturnFromException). Exceptions enter
    // the process at its entry point with x0 = the exception type and x1 =
    // the ExceptionInfo in the process local region, one thread at a time.
    void setEntryPoint(vaddr_t entry_point_) { entry_point = entry_point_; }
    // The process local region (0x200 bytes, UserExceptionContextAddress)
    vaddr_t getProcessLocalRegion();
    // False if the process has no handler, or `thread` is in an exception
    // already (a fault in the handler is not handled)
    bool enterUserException(IThread* thread,
                            hw::tegra_x1::cpu::IThread& cpu_thread,
                            const hw::tegra_x1::cpu::GuestException& info);
    // Restores the context the handler left in the ExceptionInfo. False if
    // `thread` is not handling an exception.
    bool leaveUserException(IThread* thread,
                            hw::tegra_x1::cpu::IThread& cpu_thread);

    // The application's control.nacp, if it has one
    const services::ns::ApplicationControlProperty*
    getApplicationControlProperty() const {
        return control_property.get();
    }
    void setApplicationControlProperty(
        std::unique_ptr<services::ns::ApplicationControlProperty> property);
    GETTER_AND_SETTER(ideal_core, getIdealCore, setIdealCore);
    GETTER(highest_thread_priority, getHighestThreadPriority);
    GETTER(lowest_thread_priority, getLowestThreadPriority);
    void setThreadPriorityRange(i32 highest, i32 lowest) {
        highest_thread_priority = highest;
        lowest_thread_priority = lowest;
    }
    // The priorities a thread of this process may be given
    u64 getPriorityMask() const {
        u64 mask = 0;
        for (i32 p = highest_thread_priority; p <= lowest_thread_priority; p++)
            mask |= 1ull << p;
        return mask;
    }
    bool isValidThreadPriority(i32 priority) const {
        return priority >= highest_thread_priority &&
               priority <= lowest_thread_priority;
    }
};

} // namespace hydra::horizon::kernel

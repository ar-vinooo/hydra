#include "core/horizon/kernel/shared_memory.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/system.hpp"

#include <fstream>
#include <future>
#include <iostream>
#include <sstream>

using namespace hydra;
using namespace hydra::horizon;
using hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {
constexpr u64 SIZE = 0x4000;
constexpr vaddr_t BASE = 0x32000000;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TrackedSharedMemory final : public kernel::SharedMemory {
  public:
    TrackedSharedMemory(System& system, std::atomic<bool>& destroyed_)
        : SharedMemory(system.getCpu(), SIZE), destroyed(destroyed_) {}
    ~TrackedSharedMemory() override { destroyed = true; }
  private:
    std::atomic<bool>& destroyed;
};

class Program {
  public:
    explicit Program(System& system_) : system(system_) {
        for (auto& process : processes)
            process = system.getOs().getKernel().getProcessManager().createProcess("Shared memory reference");
        for (u32 object = 0; object < 2; ++object) {
            objects[object] = new TrackedSharedMemory(system, destroyed[object]);
            weak[object] = objects[object]->getBacking();
            std::memset(reinterpret_cast<void*>(objects[object]->getPtr()),
                        object == 0 ? 0x35 : 0x7a, SIZE);
            handles[0][object] = processes[0]->addHandleNoRetain(objects[object]);
            handles[1][object] = processes[1]->addHandle(objects[object]);
        }
    }
    ~Program() {
        finishGpu();
        for (auto* process : processes)
            if (process)
                system.getOs().getKernel().getProcessManager().destroyProcess(process);
    }
    bool step(const std::string& token) {
        if (token == "B") {
            require(!submitted && processes[0], "One GPU read per lifetime program");
            auto& renderer = static_cast<hw::tegra_x1::gpu::renderer::metal::Renderer&>(system.getGpu().getRenderer());
            space = processes[0]->createGpuAddressSpace();
            const auto range = ztd::Range<vaddr_t>::fromSize(BASE, SIZE);
            const uptr gpu_address = space->mapBufferToAddressSpace(range, 0x740000);
            // A fixture-only guard prevents a broken implementation from
            // making GPU cleanup read freed memory. State checks subtract it.
            guard = weak[0].lock();
            source = renderer.getDevice()->newBuffer(
                reinterpret_cast<void*>(space->unmapAddr(gpu_address)), SIZE,
                MTL::ResourceStorageModeShared, nullptr);
            result = renderer.getDevice()->newBuffer(SIZE, MTL::ResourceStorageModeShared);
            gate = renderer.getDevice()->newSharedEvent();
            require(source && result && gate, "Real Metal lifetime resources");
            std::unique_ptr<hw::tegra_x1::gpu::renderer::ICommandBuffer> commands(renderer.createCommandBuffer());
            auto* native = static_cast<hw::tegra_x1::gpu::renderer::metal::CommandBuffer*>(commands.get());
            native->getCommandBuffer()->encodeWait(gate, 1);
            native->getBlitCommandEncoder()->copyFromBuffer(source, 0, result, 0, SIZE);
            submitted = native->getCommandBuffer()->retain();
            return true;
        }
        if (token == "F") {
            if (!submitted)
                return false;
            finishGpu();
            require(gpu_ok, "Metal reads original shared bytes after close/unmap");
            return true;
        }
        require(token.size() >= 2 && token[1] >= '0' && token[1] <= '1', "Valid process token");
        const u32 process = static_cast<u32>(token[1] - '0');
        if (!processes[process])
            return false;
        if (token[0] == 'D') {
            require(!submitted, "Finish gated GPU before synchronous process destruction");
            system.getOs().getKernel().getProcessManager().destroyProcess(processes[process]);
            processes[process] = nullptr;
            handles[process].fill(INVALID_HANDLE);
            return true;
        }
        require(token.size() >= 3 && token.back() >= '0' && token.back() <= '1', "Valid object token");
        const u32 object = static_cast<u32>(token.back() - '0');
        auto& handle = handles[process][object];
        if (token[0] == 'H') {
            if (handle.isValid() || !handles[1 - process][object].isValid())
                return false;
            handle = processes[process]->addHandle(objects[object]);
            return true;
        }
        if (!handle.isValid())
            return false;
        auto& kernel = system.getOs().getKernel();
        if (token[0] == 'C') {
            const auto closed = kernel.closeHandle(processes[process], handle) == RESULT_SUCCESS;
            handle = INVALID_HANDLE;
            return closed;
        }
        require(token.size() == 4 && token[2] >= '0' && token[2] <= '1', "Valid mapping token");
        const auto address = BASE + static_cast<u64>(token[2] - '0') * 0x10000;
        if (token[0] == 'M' || token[0] == 'N' || token[0] == 'A')
            return kernel.mapSharedMemory(processes[process], objects[object],
                address + (token[0] == 'A' ? 1u : 0u), token[0] == 'N' ? 0x1000 : SIZE,
                kernel::MemoryPermission::ReadWrite) == RESULT_SUCCESS;
        require(token[0] == 'U' || token[0] == 'V', "Known lifetime operation");
        return kernel.unmapSharedMemory(processes[process], objects[object], address,
            token[0] == 'V' ? 0x1000 : SIZE) == RESULT_SUCCESS;
    }
    void observe(std::ostream& output, bool success) const {
        u32 object_mask = 0, backing_mask = 0, mapping_mask = 0, process_mask = 0;
        std::array<u32, 4> words{};
        for (u32 object = 0; object < 2; ++object) {
            object_mask |= static_cast<u32>(destroyed[object].load()) << object;
            backing_mask |= static_cast<u32>(weak[object].use_count() >
                (object == 0 && guard ? 1 : 0)) << object;
        }
        for (u32 p = 0; p < 2; ++p) {
            if (!processes[p])
                continue;
            process_mask |= 1u << p;
            for (u32 slot = 0; slot < 2; ++slot) {
                const u32 index = p * 2 + slot;
                const auto address = BASE + slot * 0x10000;
                if (processes[p]->getMmu()->queryRegion(address).state.type == kernel::MemoryType::Shared) {
                    mapping_mask |= 1u << index;
                    std::memcpy(&words[index], reinterpret_cast<void*>(processes[p]->getMmu()->unmapAddr(address)), 4);
                }
            }
        }
        output << success << ' ' << object_mask << ' ' << backing_mask << ' '
               << mapping_mask << ' ' << process_mask;
        for (const auto word : words)
            output << ' ' << word;
        output << '\n';
    }
    void checkCleanupWaits(bool mapping_active) {
        require(step("M000") && step("B"), "Queue held GPU read of shared memory");
        if (!mapping_active)
            require(step("U000"), "Unmap shared CPU memory while GPU read is held");
        require(step("C00") && step("C10"), "Close both handles before cleanup");
        auto* process = processes[0];
        processes[0] = nullptr;
        handles[0].fill(INVALID_HANDLE);
        auto cleanup = std::async(std::launch::async, [&] {
            system.getOs().getKernel().getProcessManager().destroyProcess(process);
        });
        const bool waited = cleanup.wait_for(std::chrono::milliseconds(75)) != std::future_status::ready;
        const bool pinned = weak[0].use_count() > 1;
        const bool object_kept = !mapping_active || !destroyed[0].load();
        // Always release the GPU before an assertion can throw and join the
        // cleanup future, including when testing a broken implementation.
        finishGpu();
        cleanup.get();
        require(waited && pinned && object_kept && gpu_ok && destroyed[0].load() && weak[0].expired(),
                "Process cleanup drains actual GPU work before releasing mapped/retired shared backing");
    }
  private:
    System& system;
    std::array<std::atomic<bool>, 2> destroyed{};
    std::array<kernel::Process*, 2> processes{};
    std::array<TrackedSharedMemory*, 2> objects{};
    std::array<std::array<hydra::Handle, 2>, 2> handles{};
    std::array<std::weak_ptr<hw::tegra_x1::cpu::IMemory>, 2> weak;
    std::shared_ptr<hw::tegra_x1::cpu::IMemory> guard;
    std::shared_ptr<hw::tegra_x1::gpu::GMmu> space;
    MTL::Buffer* source{nullptr};
    MTL::Buffer* result{nullptr};
    MTL::SharedEvent* gate{nullptr};
    MTL::CommandBuffer* submitted{nullptr};
    bool gpu_ok{false};

    void finishGpu() {
        if (submitted) {
            gate->setSignaledValue(1);
            submitted->waitUntilCompleted();
            const auto* bytes = static_cast<const u8*>(result->contents());
            gpu_ok = submitted->status() == MTL::CommandBufferStatusCompleted &&
                std::all_of(bytes, bytes + SIZE, [](u8 byte) { return byte == 0x35; });
            submitted->release();
            submitted = nullptr;
        }
        if (source) { source->release(); source = nullptr; }
        if (result) { result->release(); result = nullptr; }
        if (gate) { gate->release(); gate = nullptr; }
        guard.reset();
    }
};
} // namespace

int runSharedMemoryCases(System& system, const char* input_path, const char* output_path) {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    require(bool(input) && bool(output), "Open shared memory lifetime programs");
    std::string line;
    usize count = 0;
    while (std::getline(input, line)) {
        Program program(system);
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token)
            program.observe(output, program.step(token));
        ++count;
    }
    std::cout << "PASS: " << count << " shared memory lifecycle programs with gated Metal reads\n";
    return 0;
}

void testSharedMemoryCleanup(System& system) {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    for (const bool mapping_active : {true, false}) {
        Program program(system);
        program.checkCleanupWaits(mapping_active);
    }
    std::cout << "PASS: mapped and unmapped SharedMemory survive GPU-blocked process cleanup\n";
}

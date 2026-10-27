#include "core/horizon/kernel/process.hpp"
#include "core/horizon/kernel/shared_memory.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvmap.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/system.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hydra;
using namespace hydra::horizon;
using hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
namespace nv = services::nvdrv;
namespace gpu = hw::tegra_x1::gpu;

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

nv::NvResult call(System& system, kernel::Process* process, nv::ioctl::FdBase& fd,
                  u32 type, u32 nr, std::span<const u8> request, std::span<u8> output) {
    std::vector<u8> input(request.size() + 1);
    std::memcpy(input.data() + 1, request.data(), request.size());
    nv::ioctl::IoctlContext context{.system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(input).subspan(1)),
        .out_stream = ztd::io::MemoryStream(output)};
    return fd.ioctl(context, type, nr);
}
template <usize N>
nv::NvResult callWords(System& system, kernel::Process* process, nv::ioctl::FdBase& fd,
                       u32 nr, std::array<u32, N>& words, u32 type = 1) {
    auto bytes = std::span(reinterpret_cast<u8*>(words.data()), sizeof(words));
    return call(system, process, fd, type, nr, bytes, bytes);
}
}

void testStageAMemory(System& system) {
    auto& manager = system.getOs().getKernel().getProcessManager();
    auto* process = manager.createProcess("Stage A nvmap ownership");
    process->resizeHeap(0x20000);
    auto first = std::make_unique<nv::ioctl::NvMap>();
    auto second = std::make_unique<nv::ioctl::NvMap>();
    std::array<u32, 2> create{0x10000, 0};
    expect(callWords(system, process, *first, 1, create) == nv::NvResult::Success, "Create client-owned nvmap");
    const u32 handle = create[1];
    const auto before_alloc = system.getGpu().getMap(handle).value();
    std::array<u32, 2> import{handle, 0xcccccccc};
    expect(callWords(system, process, *second, 3, import) == nv::NvResult::BadParameter &&
               import[1] == 0xcccccccc, "Unallocated ID import fails without writing output");
    std::array<u32, 3> param{handle, 1, 0xcccccccc};
    expect(callWords(system, process, *second, 9, param) == nv::NvResult::BadParameter &&
               param[2] == 0xcccccccc, "A different client cannot use a handle without import");
    std::array<u32, 8> allocate{handle, 0, 1, 0x8000, 0xfe, 0, 0, 0};
    const auto guest = kernel::HEAP_REGION.getBegin();
    std::memcpy(allocate.data() + 6, &guest, 8);
    expect(callWords(system, process, *first, 4, allocate) == nv::NvResult::Success,
           "Allocate nvmap using caller alignment and kind");
    expect(before_alloc->addr == 0, "Metadata held by a reader is immutable across allocation");
    expect(callWords(system, process, *second, 3, import) == nv::NvResult::Success && import[1] == handle &&
               callWords(system, process, *second, 3, import) == nv::NvResult::Success,
           "Repeated imports acquire independent client references");
    auto* other_process = manager.createProcess("Foreign nvmap import");
    {
        nv::ioctl::NvMap foreign;
        std::array<u32, 2> foreign_import{handle, 0xa5a5a5a5};
        expect(callWords(system, other_process, foreign, 3, foreign_import) == nv::NvResult::NotSupported &&
                   foreign_import[1] == 0xa5a5a5a5,
               "Foreign CPU backing is rejected explicitly rather than interpreted in the wrong MMU");
        expect(callWords(system, other_process, *first, 3, foreign_import) == nv::NvResult::BadParameter,
               "An nvmap client cannot migrate to another process");
    }
    manager.destroyProcess(other_process);
    const auto snapshot = system.getGpu().getMap(handle).value();
    expect(snapshot->references->load() == 3, "Two imports retain the original allocation");
    param = {handle, 2, 0};
    const auto alignment_result = callWords(system, process, *second, 9, param);
    if (alignment_result != nv::NvResult::Success || param[2] != 0x8000)
        throw std::runtime_error(fmt::format("NVMAP_PARAM_ALIGNMENT: result {}, output {:#x}, metadata {:#x}",
            static_cast<u32>(alignment_result), param[2], snapshot->alignment));
    param = {handle, 5, 0};
    expect(callWords(system, process, *second, 9, param) == nv::NvResult::Success && param[2] == 0xfe,
           "NVMAP_PARAM_KIND returns allocated metadata");
    std::array<u32, 6> release{handle, 0, 0, 0, 0, 0};
    expect(callWords(system, process, *first, 5, release) == nv::NvResult::Success &&
               release[2] == 0 && release[3] == 0 && release[4] == 0x10000 && release[5] == 0 &&
               snapshot->references->load() == 2,
           "FREE drops one client reference without returning backing still in use");
    second.reset();
    expect(!system.getGpu().getMap(handle) && snapshot->addr == guest && snapshot->kind == 0xfe &&
               snapshot->references->load() == 0,
           "FD closure releases every import while readers keep valid metadata");
    import = {handle, 0xcccccccc};
    second = std::make_unique<nv::ioctl::NvMap>();
    expect(callWords(system, process, *second, 3, import) == nv::NvResult::BadParameter,
           "Dead exported ID cannot be imported");
    create = {0x1000, 0};
    expect(callWords(system, process, *first, 1, create) == nv::NvResult::Success && create[1] != handle,
           "A stale global ID cannot alias a newly created allocation");
    const u32 newer = create[1];
    for (const auto& [nr, size] : std::array<std::pair<u32, usize>, 6>{{{1, 8}, {3, 8}, {4, 32}, {5, 24}, {9, 12}, {14, 8}}}) {
        std::array<u8, 33> input{}, output;
        for (usize length = 0; length < size; ++length) {
            output.fill(0xa5);
            expect(call(system, process, *first, 1, nr, std::span(input).first(length), output) == nv::NvResult::InvalidSize &&
                       std::all_of(output.begin(), output.end(), [](u8 b) { return b == 0xa5; }),
                   "Every truncated nvmap request is rejected without writes");
            output.fill(0xa5);
            expect(call(system, process, *first, 1, nr, std::span(input).first(size),
                       std::span(output).first(length)) == nv::NvResult::InvalidSize &&
                       std::all_of(output.begin(), output.end(), [](u8 b) { return b == 0xa5; }),
                   "Every truncated nvmap output is rejected without writes");
        }
    }
    first.reset();
    second.reset();
    expect(!system.getGpu().getMap(newer), "Owning FD closure releases its created handles");

    nv::ioctl::NvHostAsGpu space;
    std::array<u64, 5> initialize{0, 0x10000, 0x20000, 0x80000, 0x40000};
    auto init_bytes = std::span(reinterpret_cast<u8*>(initialize.data()), sizeof(initialize));
    expect(call(system, process, space, 0x41, 9, init_bytes, init_bytes) == nv::NvResult::Success,
           "Configure explicit VA allocator ranges");
    std::array<u8, 65> request{}, output{};
    u32 size = 48;
    std::memcpy(request.data() + 9, &size, 4);
    expect(call(system, process, space, 0x41, 8, std::span(request).subspan(1), std::span(output).subspan(1)) == nv::NvResult::Success,
           "Unaligned inline GET_VA_REGIONS transport works");
    std::array<nv::ioctl::VaRegion, 2> regions;
    std::memcpy(regions.data(), output.data() + 17, sizeof(regions));
    expect(regions[0].addr == 0x20000 && regions[0].page_size == 0x1000 && regions[0].pages == 32 &&
               regions[1].addr == 0x40000 && regions[1].page_size == 0x10000 && regions[1].pages == 4,
           "VA regions describe each configured allocator with page counts");
    std::array<u8, 48> extra{};
    nv::ioctl::IoctlContext query{.system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(request).subspan(1, 16)),
        .out_stream = ztd::io::MemoryStream(std::span(output).subspan(1, 16)),
        .out_buffer_stream = ztd::io::MemoryStream(extra)};
    expect(space.ioctl3(query, 0x41, 8) == nv::NvResult::Success &&
               std::memcmp(extra.data(), regions.data(), extra.size()) == 0,
           "Ioctl3 writes VA regions into its separate output buffer");
    for (usize length = 0; length < 48; ++length) {
        extra.fill(0xa5);
        query.out_buffer_stream = ztd::io::MemoryStream(std::span(extra).first(length));
        expect(space.ioctl3(query, 0x41, 8) == nv::NvResult::InvalidSize &&
                   std::all_of(extra.begin(), extra.end(), [](u8 b) { return b == 0xa5; }),
               "Truncated ioctl3 output does not receive a partial region");
    }
    std::array<u64, 3> small{(u64(0x1000) << 32) | 1, 0, 0};
    auto small_bytes = std::span(reinterpret_cast<u8*>(small.data()), sizeof(small));
    expect(call(system, process, space, 0x41, 2, small_bytes, small_bytes) == nv::NvResult::Success && small[2] == 0x20000,
           "Small-page allocation uses its advertised pool");
    std::array<u64, 3> big{(u64(0x10000) << 32) | 1, 0, 0};
    auto big_bytes = std::span(reinterpret_cast<u8*>(big.data()), sizeof(big));
    expect(call(system, process, space, 0x41, 2, big_bytes, big_bytes) == nv::NvResult::Success && big[2] == 0x40000,
           "Big-page allocation uses its advertised pool");
    std::cout << "PASS: client nvmap refs, immutable metadata, checked ABI and VA region transports\n";
}

int runDeviceSharedCases(System& system, const char* input_path, const char* output_path) {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    expect(bool(input) && bool(output), "Open DeviceShared cases");
    auto& manager = system.getOs().getKernel().getProcessManager();
    auto& renderer = static_cast<gpu::renderer::metal::Renderer&>(system.getGpu().getRenderer());
    std::string line;
    while (std::getline(input, line)) {
        auto* process = manager.createProcess("DeviceShared reference");
        auto* memory = new kernel::SharedMemory(system.getCpu(), 0x8000);
        process->addHandleNoRetain(memory);
        constexpr vaddr_t BASE = 0x33000000;
        constexpr uptr GPU_BASE = 0x800000;
        expect(system.getOs().getKernel().mapSharedMemory(process, memory, BASE, 0x8000,
                   kernel::MemoryPermission::ReadWrite) == RESULT_SUCCESS, "Map DeviceShared CPU backing");
        for (u32 page = 0; page < 8; ++page)
            std::memset(reinterpret_cast<void*>(memory->getPtr() + page * 0x1000), 0x20 + static_cast<int>(page), 0x1000);
        NS_STACK_SCOPED auto source = renderer.getDevice()->newBuffer(reinterpret_cast<void*>(memory->getPtr()),
            0x8000, MTL::ResourceStorageModeShared, nullptr);
        NS_STACK_SCOPED auto result = renderer.getDevice()->newBuffer(64, MTL::ResourceStorageModeShared);
        std::array<std::shared_ptr<gpu::GMmu>, 2> spaces{process->createGpuAddressSpace(), process->createGpuAddressSpace()};
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            const char kind = token[0];
            std::replace(token.begin(), token.end(), ':', ' ');
            std::istringstream args(token.substr(1));
            u32 as, gpu_page = 0, guest_page = 0, count = 0;
            args >> as;
            expect(as < 2, "DeviceShared address space index");
            bool success = true;
            if (kind == 'D') spaces[as].reset();
            else if (kind == 'R') spaces[as] = process->createGpuAddressSpace();
            else if (!spaces[as]) success = false;
            else if (kind == 'M') {
                args >> gpu_page >> guest_page >> count;
                expect(count && gpu_page + count <= 8 && guest_page + count <= 8, "Valid device map range");
                spaces[as]->mapBufferToAddressSpace(ztd::Range<vaddr_t>::fromSize(BASE + guest_page * 0x1000,
                    count * 0x1000), GPU_BASE + gpu_page * 0x1000);
            } else {
                args >> gpu_page >> count;
                expect(count && gpu_page + count <= 8 && (kind == 'U' || kind == 'S'), "Valid device unmap range");
                if (kind == 'U') spaces[as]->unmapRange(GPU_BASE + gpu_page * 0x1000, count * 0x1000, false);
                else spaces[as]->unbindToSparse(GPU_BASE + gpu_page * 0x1000, count * 0x1000);
            }
            u32 attributes = 0;
            std::array<u32, 8> counts{};
            for (u32 page = 0; page < 8; ++page) {
                const auto address = BASE + page * 0x1000;
                const auto info = process->getMmu()->queryMemory(address);
                expect(info.addr <= address && address - info.addr < info.size, "QueryMemory region contains queried address");
                counts[page] = info.device_ref_count;
                attributes |= static_cast<u32>(any(info.state.attr & kernel::MemoryAttribute::DeviceShared)) << page;
            }
            std::unique_ptr<gpu::renderer::ICommandBuffer> commands(renderer.createCommandBuffer());
            auto* blit = static_cast<gpu::renderer::metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder();
            blit->fillBuffer(result, NS::Range(0, 64), 0);
            for (u32 a = 0; a < 2; ++a) for (u32 page = 0; page < 8; ++page) {
                if (!spaces[a]) continue;
                uptr base;
                const auto mapping = spaces[a]->findAddr(GPU_BASE + page * 0x1000, base);
                if (mapping && mapping->guest) {
                    const auto host = mapping->ptr + GPU_BASE + page * 0x1000 - base;
                    blit->copyFromBuffer(source, host - memory->getPtr(), result, (a * 8 + page) * 4, 4);
                }
            }
            expect(renderer.synchronize(commands.get()), "Read live GPU mappings on actual Metal");
            output << success << ' ' << attributes;
            for (const auto count_value : counts) output << ' ' << count_value;
            const auto* words = static_cast<const u32*>(result->contents());
            for (u32 i = 0; i < 16; ++i) output << ' ' << words[i];
            output << '\n';
        }
        // State tokens must be safe to destroy after their CPU MMU is gone.
        manager.destroyProcess(process);
    }
    std::cout << "PASS: DeviceShared mapping programs and real Metal readback\n";
    return 0;
}

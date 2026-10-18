// Engine-level GPU tests: the macro (MME) interpreter and renderer operations
// the engines issue (clears), checked on Metal where pixels are involved.
#include "core/hw/tegra_x1/gpu/macro/interpreter/driver.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/engines/2d.hpp"
#include "core/hw/tegra_x1/gpu/engines/compute.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <map>
#include <stdexcept>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {

void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Register file and method log the macros run against
class FakeHost final : public macro::MacroHost {
  public:
    std::map<u32, u32> registers;
    std::vector<std::pair<u32, u32>> calls;
    macro::DriverBase* driver{nullptr};

    u32 readMacroRegister(u32 reg) override {
        const auto it = registers.find(reg);
        return it != registers.end() ? it->second : 0;
    }
    void callMacroMethod(u32 method, u32 arg) override {
        if (method >= MACRO_METHODS_REGION) {
            driver->pushArgument(method - MACRO_METHODS_REGION, arg);
            return;
        }
        registers[method] = arg;
        calls.emplace_back(method, arg);
    }
};

// MME instruction encoding (operation in bits 0-2)
namespace mme {
constexpr u32 EXIT = 1u << 7;
u32 alu(u32 result, u32 dst, u32 a, u32 b, u32 op) {
    return 0u | (result << 4) | (dst << 8) | (a << 11) | (b << 14) | (op << 17);
}
u32 addImmediate(u32 result, u32 dst, u32 a, i32 imm) {
    return 1u | (result << 4) | (dst << 8) | (a << 11) |
           ((static_cast<u32>(imm) & 0x3ffff) << 14);
}
u32 bitfield(u32 operation, u32 result, u32 dst, u32 a, u32 b, u32 src_bit,
             u32 size, u32 dst_bit) {
    return operation | (result << 4) | (dst << 8) | (a << 11) | (b << 14) |
           (src_bit << 17) | (size << 22) | (dst_bit << 27);
}
u32 read(u32 result, u32 dst, u32 a, i32 imm) {
    return 5u | (result << 4) | (dst << 8) | (a << 11) |
           ((static_cast<u32>(imm) & 0x3ffff) << 14);
}
u32 branch(bool not_zero, bool annul, u32 a, i32 imm) {
    return 7u | (u32(not_zero) << 4) | (u32(annul) << 5) | (a << 11) |
           ((static_cast<u32>(imm) & 0x3ffff) << 14);
}
// Result operations
constexpr u32 FETCH = 0, MOVE = 1, MOVE_SET_METHOD = 2, FETCH_SEND = 3,
              MOVE_SEND = 4, FETCH_SET_METHOD = 5, MOVE_SET_METHOD_FETCH_SEND = 6,
              MOVE_SET_METHOD_SEND = 7;
} // namespace mme

struct MacroRun {
    FakeHost host;
    macro::interpreter::Driver driver{host};

    explicit MacroRun(const std::vector<std::vector<u32>>& macros) {
        host.driver = &driver;
        u32 address = 0;
        driver.loadInstructionRamPointer(0);
        driver.loadStartAddressRamPointer(0);
        for (const auto& code : macros) {
            driver.loadStartAddressRam(address);
            for (const auto word : code)
                driver.loadInstructionRam(word);
            address += static_cast<u32>(code.size());
        }
    }

    void call(u32 macro, const std::vector<u32>& params) {
        for (usize i = 0; i < params.size(); ++i)
            driver.pushArgument(macro * 2 + (i == 0 ? 0 : 1), params[i]);
        driver.execute();
    }
};

void testMacroAlu() {
    using namespace mme;
    // r2 = p0 + p1 (carry), r3 = 0 + 0 + carry, r4 = p0 - p1 (borrow flag),
    // r5 = 0 - 0 - !carry; results sent to methods 0x100..0x103
    const std::vector<u32> code{
        alu(FETCH, 2, 0, 0, 0),          // r2 = p1
        alu(MOVE, 4, 1, 2, 2),           // r4 = r1 - r2, carry = no borrow
        alu(MOVE, 5, 0, 0, 3),           // r5 = 0 - 0 - !carry
        alu(MOVE, 2, 1, 2, 0),           // r2 = r1 + r2, carry out
        alu(MOVE, 3, 0, 0, 1),           // r3 = 0 + 0 + carry
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x100 | (1 << 12)),
        alu(MOVE_SEND, 0, 2, 0, 9),      // send r2 | 0
        alu(MOVE_SEND, 0, 3, 0, 9),
        alu(MOVE_SEND, 0, 4, 0, 9),
        alu(MOVE_SEND | 0, 0, 5, 0, 9) | EXIT,
        alu(MOVE, 0, 0, 0, 0),           // delay slot
    };
    const std::pair<u32, u32> cases[] = {{1, 2}, {0xffffffffu, 1},
                                         {0x80000000u, 0x80000000u}, {5, 5},
                                         {0, 0xffffffffu}};
    for (const auto& [a, b] : cases) {
        MacroRun run({code});
        run.call(0, {a, b});
        expect(run.host.calls.size() == 4, "ALU macro sends four results");
        const u64 sum = u64(a) + b;
        const bool sub_carry = a >= b;
        expect(run.host.calls[0] == std::make_pair(0x100u, u32(sum)),
               "Add result");
        expect(run.host.calls[1].second == (sum > 0xffffffffu ? 1u : 0u),
               "Add carry propagates through AddWithCarry");
        expect(run.host.calls[2].second == a - b, "Subtract result");
        // r5 = 0 - 0 - (carry ? 0 : 1)
        expect(run.host.calls[3].second == (sub_carry ? 0u : 0xffffffffu),
               "Subtract sets carry when no borrow occurs");
    }
    std::cout << "PASS: macro ADD/ADDC/SUB/SUBB with unsigned carry and borrow\n";
}

void testMacroBitfields() {
    using namespace mme;
    // r2 = p0, r3 = p1, then
    //   ExtractShiftLeftImmediate: (r3 >> r2) & mask(4) << 8
    //   ExtractShiftLeftRegister:  (r3 >> 4) & mask(4) << r2
    //   ExtractInsert: insert r3[7:4] into r1 at bit 12
    const std::vector<u32> code{
        alu(FETCH, 3, 0, 0, 0),
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x200 | (1 << 12)),
        bitfield(3, MOVE_SEND, 4, 1, 3, 0, 4, 8),
        bitfield(4, MOVE_SEND, 5, 1, 3, 4, 4, 0),
        bitfield(2, MOVE_SEND, 6, 1, 3, 4, 4, 12) | EXIT,
        alu(MOVE, 0, 0, 0, 0),
    };
    for (const u32 shift : {0u, 3u, 8u, 31u}) {
        const u32 value = 0xa5c3f00fu;
        MacroRun run({code});
        run.call(0, {shift, value});
        expect(run.host.calls.size() == 3, "Bitfield macro sends three results");
        expect(run.host.calls[0].second == (((value >> shift) & 0xf) << 8),
               "ExtractShiftLeftImmediate extracts B at the position in A");
        expect(run.host.calls[1].second == (((value >> 4) & 0xf) << shift),
               "ExtractShiftLeftRegister shifts B's field left by A");
        expect(run.host.calls[2].second ==
                   ((shift & ~(0xfu << 12)) | (((value >> 4) & 0xf) << 12)),
               "ExtractInsert replaces a field of A with a field of B");
    }
    std::cout << "PASS: macro extract/insert operand order\n";
}

void testMacroControlFlow() {
    using namespace mme;
    // Loop: send p0 to methods 0x300.. while counting r1 down, then read a
    // register at a negative immediate offset
    const std::vector<u32> loop{
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x300 | (1 << 12)),
        addImmediate(MOVE, 2, 0, 0x7),             // r2 = value
        // loop:
        alu(MOVE_SEND, 0, 2, 0, 9),                // send r2
        addImmediate(MOVE, 1, 1, -1),              // r1--
        branch(true, false, 1, -2),                // bnz r1, loop (delay slot)
        addImmediate(MOVE, 2, 2, 1),               // delay slot: r2++
        read(MOVE, 3, 0, 0x404),
        read(MOVE, 4, 3, -0x10),                   // reg[r3 - 0x10]
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x310),
        alu(MOVE_SEND, 0, 4, 0, 9) | EXIT,
        addImmediate(MOVE_SEND, 0, 0, 0x55),       // exit delay slot sends
        addImmediate(MOVE_SEND, 0, 0, 0x66),       // never executes
    };
    MacroRun run({loop});
    run.host.registers[0x404] = 0x420;
    run.host.registers[0x410] = 0xbeef;
    run.call(0, {3});
    std::vector<std::pair<u32, u32>> expected{
        {0x300, 7}, {0x301, 8}, {0x302, 9}, {0x310, 0xbeef}, {0x310, 0x55}};
    expect(run.host.calls == expected,
           "Branch delay slots, negative read offsets and exit delay slot");

    // Annulled branches skip the delay slot; an exit in a delay slot does not
    // end the macro
    const std::vector<u32> annul{
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x320),
        branch(false, true, 0, 3),                 // bz r0 (taken), annul
        addImmediate(MOVE_SEND, 0, 0, 1),          // skipped
        addImmediate(MOVE_SEND, 0, 0, 2),          // skipped
        branch(false, false, 0, 3),                // taken with delay slot
        addImmediate(MOVE_SEND, 0, 0, 3) | EXIT,   // delay slot exit ignored
        addImmediate(MOVE_SEND, 0, 0, 4),          // skipped by the branch
        addImmediate(MOVE_SEND, 0, 0, 5) | EXIT,
        alu(MOVE, 0, 0, 0, 0),
    };
    MacroRun annul_run({annul});
    annul_run.call(0, {0});
    expected = {{0x320, 3}, {0x320, 5}};
    expect(annul_run.host.calls == expected,
           "Annulled branches skip their delay slot, delay slot exits are ignored");
    std::cout << "PASS: macro branches, delay slots, annul, exit delay slots and "
                 "signed read offsets\n";
}

void testMacroParametersAndNesting() {
    using namespace mme;
    // Macro 0 binds a method from its first parameter and sends the rest,
    // then calls macro 1 with one parameter
    const std::vector<u32> send_all{
        alu(MOVE_SET_METHOD, 0, 1, 0, 9),           // method = p0
        alu(FETCH, 2, 0, 0, 0),                     // r2 = count
        // loop:
        alu(FETCH, 3, 0, 0, 0),                     // r3 = next parameter
        alu(MOVE_SEND, 0, 3, 0, 9),                 // send r3
        addImmediate(MOVE, 2, 2, -1),
        branch(true, false, 2, -3),
        alu(MOVE, 0, 0, 0, 0),                      // delay slot
        addImmediate(MOVE_SET_METHOD, 0, 0, MACRO_METHODS_REGION + 2),
        addImmediate(MOVE_SEND, 0, 0, 0x77) | EXIT, // call macro 1
        alu(MOVE, 0, 0, 0, 0),
    };
    const std::vector<u32> callee{
        addImmediate(MOVE_SET_METHOD, 0, 0, 0x500),
        alu(MOVE_SEND, 0, 1, 0, 9) | EXIT,          // send p0
        alu(MOVE, 0, 0, 0, 0),
    };
    MacroRun run({send_all, callee});
    run.call(0, {0x400 | (1 << 12), 3, 10, 11, 12});
    std::vector<std::pair<u32, u32>> expected{
        {0x400, 10}, {0x401, 11}, {0x402, 12}, {0x500, 0x77}};
    expect(run.host.calls == expected,
           "Parameters are fetched in order and a nested macro runs in place");

    // A second call starts from clean state (registers, carry, method)
    run.host.calls.clear();
    run.call(1, {0x99});
    expected = {{0x500, 0x99}};
    expect(run.host.calls == expected, "Each call starts with fresh state");

    // Every argument after the first is a parameter, whichever macro method
    // receives it (non-incrementing and incrementing bursts)
    run.host.calls.clear();
    for (const u32 arg : {0x400u | (1u << 12), 2u, 20u, 21u})
        run.driver.pushArgument(0, arg); // all to the call method
    run.driver.execute();
    expected = {{0x400, 20}, {0x401, 21}, {0x500, 0x77}};
    expect(run.host.calls == expected,
           "Arguments of a non-incrementing burst are parameters");
    std::cout << "PASS: macro parameters, bursts, nested macro calls and per-call "
                 "state\n";
}

void testMacroResultOperations() {
    using namespace mme;
    const std::vector<u32> code{
        // method = 0x600, send p1
        addImmediate(MOVE_SET_METHOD_FETCH_SEND, 2, 0, 0x600),
        // method = 0x610, send bits 12..17 of the value (5)
        addImmediate(MOVE_SET_METHOD_SEND, 0, 0, 0x610 | (5 << 12)),
        // r3 = p2, method = p0
        alu(FETCH_SET_METHOD, 3, 1, 0, 9),
        alu(MOVE_SEND, 0, 3, 0, 9), // send r3
        // send the result 0x33 while fetching p3 into r4, then send r4
        addImmediate(FETCH_SEND, 4, 0, 0x33),
        alu(MOVE_SEND, 0, 4, 0, 9) | EXIT,
        alu(MOVE, 0, 0, 0, 0),
    };
    MacroRun run({code});
    run.call(0, {0x620, 0x11, 0x22, 0x44});
    const std::vector<std::pair<u32, u32>> expected{
        {0x600, 0x11}, {0x610, 5}, {0x620, 0x22}, {0x620, 0x33}, {0x620, 0x44}};
    expect(run.host.calls == expected,
           "Set-method/fetch/send result operations");
    std::cout << "PASS: macro result operations\n";
}

// Copies a (possibly private) texture's first level to host memory
std::vector<u8> readTexture(MTL::Device* device, MTL::Texture* texture,
                            u32 bytes_per_pixel) {
    const auto width = texture->width();
    const auto height = texture->height();
    NS_STACK_SCOPED auto buffer = device->newBuffer(
        width * height * bytes_per_pixel, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* blit = commands->blitCommandEncoder();
    blit->copyFromTexture(texture, 0, 0, MTL::Origin(0, 0, 0),
                          MTL::Size(width, height, 1), buffer, 0,
                          width * bytes_per_pixel, width * height * bytes_per_pixel);
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    const auto* data = static_cast<const u8*>(buffer->contents());
    return std::vector<u8>(data, data + width * height * bytes_per_pixel);
}

// Clears only depend on the clear state: a color clear after a depth clear
// keeps the depth, and every clear is limited to the clear area
// Storage buffers wrap guest memory pages: GPU writes must appear in the
// guest's bytes, and a range inside a page must keep its neighbours intact
void testGuestMemoryBuffer() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    const auto page_size = static_cast<usize>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<u8*>(mmap(nullptr, 3 * page_size,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    expect(memory != MAP_FAILED, "Guest memory allocation");
    std::memset(memory, 0xcd, 3 * page_size);

    metal::Renderer renderer;
    // 32 bytes straddling the first page boundary, 16 byte aligned
    const auto range = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(memory) + page_size - 16, 32);
    const auto view = renderer.getGuestMemoryBuffer(range);
    expect(view.isValid(), "Guest memory buffer is created");
    expect(view.getSize() == 32, "View covers the requested bytes");
    // The same pages are shared by a second request
    const auto again = renderer.getGuestMemoryBuffer(range);
    expect(again.getBase() == view.getBase(), "Guest memory buffers are cached");

    NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
    const char* source = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void fill(device uint* b [[buffer(0)]],
                         uint t [[thread_position_in_grid]]) { b[t] = t + 1u; })";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library = device->newLibrary(
        NS::String::string(source, NS::UTF8StringEncoding), nullptr, &error);
    NS_STACK_SCOPED auto function = library->newFunction(
        NS::String::string("fill", NS::UTF8StringEncoding));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, "Fill kernel compiles");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(static_cast<metal::Buffer*>(view.getBase())->getBuffer(),
                       view.getOffset(), 0);
    encoder->dispatchThreads(MTL::Size(8, 1, 1), MTL::Size(8, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted,
           "Kernel writes guest memory");

    const u8* written = reinterpret_cast<const u8*>(range.getBegin());
    for (u32 i = 0; i < 8; ++i) {
        u32 value;
        std::memcpy(&value, written + i * 4, 4);
        expect(value == i + 1, "GPU writes reach the guest bytes");
    }
    expect(written[-1] == 0xcd && written[32] == 0xcd,
           "Bytes next to the range are untouched");
    // Ranges a shader writes are served as views of the live memory, other
    // ranges keep the CPU snapshots
    std::unique_ptr<ICommandBuffer> cache_commands(renderer.createCommandBuffer());
    auto& buffer_cache = renderer.getBufferCache();
    const auto gpu_written_range = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(memory) + 2 * page_size, 256);
    const auto untouched = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(memory) + 64, 256);
    {
        std::scoped_lock lock(buffer_cache.getMutex());
        buffer_cache.markGpuModified(gpu_written_range);
        expect(buffer_cache.get(cache_commands.get(), gpu_written_range).getPtr() == gpu_written_range.getBegin(),
               "GPU-gpu_written_range ranges read the guest memory directly");
        expect(buffer_cache.get(cache_commands.get(), untouched).getPtr() != untouched.getBegin(),
               "Other ranges are still snapshots");
    }
    cache_commands.reset();
    munmap(memory, 3 * page_size);
    std::cout << "PASS: storage buffers alias guest memory\n";
}

void testRendererClears() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    setenv("HYDRA_GPU_SYNC", "1", 1);
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    std::vector<u8> guest_memory(0x20000, 0);
    metal::Renderer renderer;
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    auto& cache = renderer.getTextureCache();
    const auto ptr = reinterpret_cast<uptr>(guest_memory.data());
    auto* color = cache.find(commands.get(),
                             TextureDescriptor(ptr, MTL::TextureType2D,
                                               TextureFormat::RGBA8Unorm, false,
                                               0, 8, 8, 1, 1, 1, 0, 0, 0),
                             TextureUsage::Write);
    auto* depth = cache.find(commands.get(),
                             TextureDescriptor(ptr + 0x10000, MTL::TextureType2D,
                                               TextureFormat::Z32Float, false, 0,
                                               8, 8, 1, 1, 1, 0, 0, 0),
                             TextureUsage::Write);
    RenderPassDescriptor pass_descriptor{};
    pass_descriptor.color_targets[0].texture = color;
    pass_descriptor.depth_stencil_target.texture = depth;
    std::unique_ptr<RenderPassBase> pass(
        renderer.createRenderPass(pass_descriptor));
    renderer.bindRenderPass(pass.get());
    const MTL::ScissorRect full(0, 0, 8, 8);
    renderer.setClearState({.rect = full});
    renderer.clearColor(commands.get(), 0, 0, 0xf, simd::uint4{0, 0, 0, 0});
    renderer.clearDepth(commands.get(), 0, 0.75f);
    // Partial color clear right after the depth clear
    renderer.setClearState({.rect = MTL::ScissorRect(2, 1, 4, 2)});
    renderer.clearColor(commands.get(), 0, 0, 0xf,
                        simd::uint4{std::bit_cast<u32>(1.0f), 0,
                                    std::bit_cast<u32>(1.0f),
                                    std::bit_cast<u32>(1.0f)});
    commands.reset();

    auto* device = static_cast<metal::TextureView*>(color)->getTexture()->device();
    const auto color_pixels = readTexture(
        device, static_cast<metal::TextureView*>(color)->getTexture(), 4);
    const auto depth_pixels = readTexture(
        device, static_cast<metal::TextureView*>(depth)->getTexture(), 4);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool inside = x >= 2 && x < 6 && y >= 1 && y < 3;
            const u8* p = &color_pixels[(y * 8 + x) * 4];
            expect((p[0] == 255 && p[2] == 255) == inside &&
                       (p[0] == 0) == !inside,
                   "Color clears are limited to the clear rect");
            f32 value;
            std::memcpy(&value, &depth_pixels[(y * 8 + x) * 4], 4);
            expect(value == 0.75f,
                   "A color clear does not write depth with the state of a "
                   "previous depth clear");
        }
    }
    std::cout << "PASS: renderer clears use the clear area and leave other "
                 "attachments untouched\n";
}

void testOcclusionReports() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    auto& engine = gpu.get3DEngine();
    auto* device = renderer.getDevice();
    std::vector<u8> image_memory(0x20000, 0);
    std::array<u8, 32> report_memory{};
    GMmu gmmu(nullptr);
    constexpr u64 REPORT = 0x400000;
    gmmu.map(REPORT, {reinterpret_cast<uptr>(report_memory.data()), report_memory.size()});
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    auto& cache = renderer.getTextureCache();
    const auto ptr = reinterpret_cast<uptr>(image_memory.data());
    auto* color = cache.find(commands.get(),
        TextureDescriptor(ptr, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                          false, 0, 4, 4, 1, 1, 1, 0, 0, 0), TextureUsage::Write);
    auto* depth = cache.find(commands.get(),
        TextureDescriptor(ptr + 0x10000, MTL::TextureType2D, TextureFormat::Z32Float,
                          false, 0, 4, 4, 1, 1, 1, 0, 0, 0), TextureUsage::Write);
    RenderPassDescriptor pass_descriptor{};
    pass_descriptor.color_targets[0].texture = color;
    pass_descriptor.depth_stencil_target.texture = depth;
    std::unique_ptr<RenderPassBase> pass(renderer.createRenderPass(pass_descriptor));
    renderer.bindRenderPass(pass.get());
    const auto shader = [&](ShaderType type, const std::string& code) {
        ShaderDescriptor descriptor{};
        descriptor.type = type;
        descriptor.backend = ShaderBackend::Msl;
        descriptor.code.assign(code.begin(), code.end());
        return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
    };
    const auto vertex = shader(ShaderType::Vertex, R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]]) {
    float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[id], 0.5, 1);
})");
    const auto fragment = shader(ShaderType::Fragment, R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 main_() { return float4(1); }
)");
    PipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Vertex)] = vertex.get();
    pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Fragment)] = fragment.get();
    pipeline_descriptor.color_target_states[0].format = TextureFormat::RGBA8Unorm;
    pipeline_descriptor.color_target_states[0].write_mask = engines::ColorWriteMask::All;
    pipeline_descriptor.depth_stencil_format = TextureFormat::Z32Float;
    std::unique_ptr<PipelineBase> pipeline(renderer.createPipeline(pipeline_descriptor));
    renderer.bindPipeline(pipeline.get());
    const MTL::ScissorRect full(0, 0, 4, 4);
    for (u32 i = 0; i < VIEWPORT_COUNT; i++) {
        renderer.setViewport(i, MTL::Viewport{0, 0, 4, 4, 0, 1});
        renderer.setScissor(i, full);
    }
    renderer.setCullState(false, engines::CullFaceMode::Back, engines::Winding::CounterClockwise);
    renderer.setDepthTestEnabled(true);
    renderer.setDepthWriteEnabled(false);
    renderer.setDepthCompareOp(engines::CompareOp::Less);
    renderer.setStencilState({});
    renderer.setClearState({.rect = full});
    renderer.clearDepth(commands.get(), 0, 0.25f);
    tls_crnt_gmmu = &gmmu;
    renderer.setOcclusionCounterEnabled(true);
    engine.method(0x54c, 1);
    const auto draw = [&] {
        renderer.draw(commands.get(), engines::PrimitiveType::Triangles, 0, 3, 0, 1);
    };
    const auto report = [&](u32 counter, bool short_query = false) {
        engine.method(0x6c0, 0);
        engine.method(0x6c1, static_cast<u32>(REPORT));
        engine.method(0x6c3, 2 | (counter << 23) | (static_cast<u32>(short_query) << 28));
        u64 value = 0;
        std::memcpy(&value, report_memory.data(), short_query ? 4 : 8);
        return value;
    };
    draw();
    expect(report(0x15) == 0, "Fully occluded geometry reports zero samples");
    std::copy_n(report_memory.data(), 16, report_memory.data() + 16);
    renderer.clearDepth(commands.get(), 0, 0.75f);
    draw();
    expect(report(0x15) == 16, "Visible geometry reports pixels, not draw calls");
    engine.method(0x554, 0);
    engine.method(0x555, static_cast<u32>(REPORT));
    engine.method(0x556, 3);
    expect(!engine.shouldExecute(), "Real occlusion reports drive unequal render conditions");
    engine.method(0x556, 4);
    expect(engine.shouldExecute(), "A visible report differs from a zero-sample report");
    engine.method(0x556, 1);
    u64 first_timestamp;
    std::memcpy(&first_timestamp, report_memory.data() + 8, 8);
    expect(first_timestamp != 0, "Long reports include a completion timestamp");
    std::unique_ptr<BufferBase> indices(renderer.allocateTemporaryBuffer(6));
    const u16 triangle[] = {0, 1, 2};
    std::memcpy(reinterpret_cast<void*>(indices->getPtr()), triangle, sizeof(triangle));
    const u32 guard_word = 0xcafebabe;
    std::memcpy(report_memory.data() + 4, &guard_word, sizeof(guard_word));
    renderer.bindIndexBuffer(BufferView(indices.get()), engines::IndexType::UInt16);
    renderer.drawIndexed(commands.get(), engines::PrimitiveType::Triangles, 0, 3, 0, 0, 1);
    expect(report(0x2, true) == 32, "Indexed draws add their visible samples to the counter");
    u32 kept_word;
    u64 kept_timestamp;
    std::memcpy(&kept_word, report_memory.data() + 4, 4);
    std::memcpy(&kept_timestamp, report_memory.data() + 8, 8);
    expect(kept_word == guard_word && kept_timestamp == first_timestamp,
           "Short reports leave the following words and timestamp untouched");
    renderer.setOcclusionCounterEnabled(false);
    draw();
    expect(report(0x15) == 32, "Disabled counters preserve the accumulated value");
    engine.method(0x54c, 1);
    expect(report(0x15) == 0, "Counter reset clears the accumulated result");
    renderer.setOcclusionCounterEnabled(true);
    draw();
    engine.method(0x54c, 1); // Drop an in-flight epoch before committing it.
    draw();
    expect(report(0x15) == 16, "Reset does not include draws from the prior in-flight epoch");
    engine.method(0x54c, 1);
    draw();
    commands.reset();
    commands.reset(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    draw();
    const auto across_submissions = report(0x15);
    expect(across_submissions == 32,
           fmt::format("Queries accumulate across command-buffer submissions (got {})",
                       across_submissions).c_str());
    engine.method(0x54c, 1);
    draw();
    renderer.clearColor(commands.get(), 0, 0, 0xf, simd::uint4{0, 0, 0, 0});
    renderer.clearDepth(commands.get(), 0, 0.75f);
    draw();
    expect(report(0x15) == 32, "Clear passes preserve earlier counts and add no samples");
    engine.method(0x54c, 1);
    for (u32 i = 0; i < VIEWPORT_COUNT; i++)
        renderer.setScissor(i, MTL::ScissorRect(0, 0, 2, 2));
    draw();
    expect(report(0x15) == 4, "Scissor coverage determines the actual sample count");
    for (u32 i = 0; i < VIEWPORT_COUNT; i++)
        renderer.setScissor(i, full);
    engine.method(0x54c, 1);
    for (u32 i = 0; i < 5000; i++)
        draw();
    expect(report(0x15) == 80000, "Large query batches survive visibility-buffer rollover");
    engine.method(0x54c, 1);
    for (u32 i = 0; i < 160; i++) {
        draw();
        static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder();
    }
    expect(report(0x15) == 2560,
           "Long-lived counters fold completed pages without losing samples");

    NS_STACK_SCOPED auto witness = device->newBuffer(16, MTL::ResourceStorageModeShared);
    std::memset(witness->contents(), 0, 16);
    auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
    metal_commands->getBlitCommandEncoder()->fillBuffer(witness, NS::Range(0, 16), 0x5a);
    auto completion = std::make_shared<std::promise<void>>();
    auto finished = completion->get_future();
    commands->onCompleted([completion] { completion->set_value(); });
    engine.method(0x6c0, 0xffffff00); // Reserved upper-address bits are ignored.
    engine.method(0x6c2, 0x12345678);
    engine.method(0x6c3, 1u << 28); // Short payload release.
    u32 payload;
    std::memcpy(&payload, report_memory.data(), 4);
    expect(payload == 0x12345678 && static_cast<const u8*>(witness->contents())[0] == 0x5a,
           "A semaphore payload is published only after preceding GPU writes finish");
    expect(finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
           "A report checkpoint does not complete the whole logical submission");
    std::array<u8, 4> split_head{};
    std::array<u8, 12> split_tail{};
    gmmu.map(REPORT + 0x100, {reinterpret_cast<uptr>(split_head.data()), 4});
    gmmu.map(REPORT + 0x104, {reinterpret_cast<uptr>(split_tail.data()), 12});
    engine.method(0x6c1, static_cast<u32>(REPORT + 0x100));
    engine.method(0x6c2, 0x87654321);
    engine.method(0x6c3, 0);
    const auto split_report = gmmu.readMappedBytes(REPORT + 0x100, 16);
    u64 split_value, split_time;
    std::memcpy(&split_value, split_report.data(), 8);
    std::memcpy(&split_time, split_report.data() + 8, 8);
    expect(split_value == 0x87654321 && split_time >= first_timestamp,
           "Long reports span separate host mappings without losing the timestamp");
    gmmu.unmap(REPORT + 0x104);
    engine.method(0x6c2, 0xdeadbeef);
    engine.method(0x6c3, 0);
    u32 unchanged;
    std::memcpy(&unchanged, split_head.data(), 4);
    expect(unchanged == 0x87654321, "Unmapped report tails do not cause partial writes");
    renderer.setOcclusionCounterEnabled(false);
    commands.reset();
    expect(finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
           "Logical completion callbacks remain attached to final submission");
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: real occlusion counts, reset epochs, rollover and completion-ordered reports\n";
}

// Release semaphores with a reduction combine the payload with memory, and
// acquires wait for the value another writer releases
void testReportSemaphoreReductionAndAcquire() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    auto& engine = gpu.get3DEngine();
    std::array<u32, 8> memory{};
    GMmu gmmu(nullptr);
    constexpr u64 SEMAPHORE = 0x500000;
    gmmu.map(SEMAPHORE, {reinterpret_cast<uptr>(memory.data()), sizeof(memory)});
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    tls_crnt_gmmu = &gmmu;
    engine.method(0x6c0, 0);
    engine.method(0x6c1, static_cast<u32>(SEMAPHORE));

    const auto reduce = [&](u32 op, u32 format, u32 initial, u32 payload) {
        memory[0] = initial;
        engine.method(0x6c2, payload);
        // Short release with reduction enabled
        engine.method(0x6c3, (1u << 28) | (1u << 3) | (op << 9) | (format << 17));
        return memory[0];
    };
    expect(reduce(0, 0, 10, 5) == 15, "Release reduction ADD");
    expect(reduce(1, 0, 10, 5) == 5, "Release reduction MIN unsigned");
    expect(reduce(1, 1, 0xfffffffeu, 3) == 0xfffffffeu,
           "Release reduction MIN signed treats the high bit as a sign");
    expect(reduce(2, 0, 10, 5) == 10, "Release reduction MAX unsigned");
    expect(reduce(3, 0, 3, 3) == 0 && reduce(3, 0, 2, 3) == 3,
           "Release reduction INC wraps at the payload");
    expect(reduce(4, 0, 0, 7) == 7 && reduce(4, 0, 5, 7) == 4,
           "Release reduction DEC wraps to the payload");
    expect(reduce(5, 0, 0xf0f0, 0xff00) == 0xf000, "Release reduction AND");
    expect(reduce(6, 0, 0xf0, 0x0f) == 0xff, "Release reduction OR");
    expect(reduce(7, 0, 0xff, 0x0f) == 0xf0, "Release reduction XOR");
    memory[1] = 0x77777777;
    engine.method(0x6c2, 1);
    memory[0] = 1;
    engine.method(0x6c3, (1u << 28) | (1u << 3));
    expect(memory[0] == 2 && memory[1] == 0x77777777,
           "A reduction does not write a timestamp or neighbouring words");

    // Acquire returns at once when the value is already there
    memory[0] = 42;
    engine.method(0x6c2, 42);
    engine.method(0x6c3, 1);
    // GE accepts a larger value, wrapping like a signed difference
    memory[0] = 50;
    engine.method(0x6c3, 1 | (1u << 16));
    // And waits for a writer on another thread
    memory[0] = 0;
    engine.method(0x6c2, 7);
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        std::atomic_ref<u32>(memory[0]).store(7, std::memory_order_release);
    });
    const auto start = std::chrono::steady_clock::now();
    engine.method(0x6c3, 1);
    const auto waited = std::chrono::steady_clock::now() - start;
    writer.join();
    expect(waited >= std::chrono::milliseconds(40) &&
               waited < std::chrono::milliseconds(900),
           "Acquire waits for the semaphore release");
    commands.reset();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: report semaphore reduction and acquire\n";
}

void testConditionalRendering() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& engine = gpu.get3DEngine();
    std::array<u32, 6> compare{5, 1, 0xdead, 0xbeef, 5, 1};
    GMmu gmmu(nullptr);
    constexpr u64 ADDRESS = 0x100400000ull;
    // The comparison spans mappings and uses the high address register.
    gmmu.map(ADDRESS, {reinterpret_cast<uptr>(compare.data()), 12});
    gmmu.map(ADDRESS + 12, {reinterpret_cast<uptr>(compare.data()) + 12, 12});
    tls_crnt_gmmu = &gmmu;
    engine.method(0x554, static_cast<u32>(ADDRESS >> 32));
    engine.method(0x555, static_cast<u32>(ADDRESS));
    engine.method(0x651, 0);
    engine.method(0x556, 0);
    expect(!engine.shouldExecute(), "FALSE rendering mode disables execution");
    engine.method(0x556, 1);
    expect(engine.shouldExecute(), "TRUE rendering mode enables execution");
    engine.method(0x556, 2);
    expect(engine.shouldExecute(), "Conditional mode requires both initial words nonzero");
    compare[0] = 0;
    expect(!engine.shouldExecute(), "Conditional mode rejects a zero initial sequence");
    compare[0] = 5;
    compare[1] = 0;
    expect(!engine.shouldExecute(), "Conditional mode rejects a zero initial mode");
    compare[1] = 1;
    engine.method(0x556, 3);
    expect(engine.shouldExecute(), "RENDER_IF_EQUAL compares both report words");
    compare[5] = 2;
    expect(!engine.shouldExecute(), "Equal mode rejects a mode-word mismatch");
    engine.method(0x556, 4);
    expect(engine.shouldExecute(), "RENDER_IF_NOT_EQUAL accepts either word differing");
    compare[5] = 1;
    expect(!engine.shouldExecute(), "Not-equal mode rejects matching report words");
    compare[4] = 6;
    expect(engine.shouldExecute(), "Not-equal mode accepts a sequence-word mismatch");
    engine.method(0x651, 2);
    expect(!engine.shouldExecute(), "NEVER override wins over comparison results");
    engine.method(0x556, 0);
    engine.method(0x651, 1);
    expect(engine.shouldExecute(), "ALWAYS override wins over FALSE mode");
    engine.method(0x651, 0);
    engine.method(0x556, 3);
    gmmu.unmap(ADDRESS + 12);
    expect(!engine.shouldExecute(), "Incomplete comparison data safely disables execution");
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: conditional render comparisons, high addresses and overrides\n";
}

// Decoded index buffers follow the guest indices: rewriting them at the same
// address must not reuse the previous decode
void testIndexDecoding() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    metal::Renderer renderer;
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    auto& cache = renderer.getIndexCache();
    std::vector<u8> indices = {3, 1, 4, 1, 5, 9, 2, 6};
    const auto range = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(indices.data()), indices.size());
    const auto decode = [&](engines::PrimitiveType primitive, u32 count,
                            engines::IndexType& type,
                            engines::PrimitiveType& out_primitive,
                            u32& out_count) {
        return cache.decode(commands.get(),
                            {.type = engines::IndexType::UInt8,
                             .primitive_type = primitive,
                             .count = count,
                             .mem_range = range},
                            type, out_primitive, out_count);
    };
    const auto read_u16 = [](const BufferView& view, u32 count) {
        std::vector<u16> values(count);
        std::memcpy(values.data(), reinterpret_cast<const void*>(view.getPtr()),
                    count * sizeof(u16));
        return values;
    };

    engines::IndexType type;
    engines::PrimitiveType primitive;
    u32 count;
    const auto first = decode(engines::PrimitiveType::Points, 8, type,
                              primitive, count);
    expect(type == engines::IndexType::UInt16 && count == 8,
           "U8 indices decode to U16");
    expect(read_u16(first, 8) == std::vector<u16>({3, 1, 4, 1, 5, 9, 2, 6}),
           "U8 indices are widened");
    expect(decode(engines::PrimitiveType::Points, 8, type, primitive, count)
                   .getBase() == first.getBase(),
           "Unchanged indices reuse their decode");

    indices[2] = 42;
    const auto rewritten = decode(engines::PrimitiveType::Points, 8, type,
                                  primitive, count);
    expect(rewritten.getBase() != first.getBase() &&
               read_u16(rewritten, 8)[2] == 42,
           "Rewritten indices are decoded again");
    expect(read_u16(first, 8)[2] == 4,
           "A decode in use is not overwritten");

    // indices: 3 1 42 1 5 9 2 6
    const auto quads = decode(engines::PrimitiveType::Quads, 8, type, primitive,
                              count);
    expect(primitive == engines::PrimitiveType::Triangles && count == 12,
           "Quads become two triangles each");
    expect(read_u16(quads, 12) ==
               std::vector<u16>({3, 1, 42, 3, 42, 1, 5, 9, 2, 5, 2, 6}),
           "U8 quads keep their indices and winding");
    const auto fan = decode(engines::PrimitiveType::TriangleFan, 5, type,
                            primitive, count);
    expect(primitive == engines::PrimitiveType::Triangles && count == 9 &&
               read_u16(fan, 9) ==
                   std::vector<u16>({3, 1, 42, 3, 42, 1, 3, 1, 5}),
           "Indexed fans become triangles around their first index");
    const auto strip = decode(engines::PrimitiveType::QuadStrip, 6, type,
                              primitive, count);
    expect(count == 12 && read_u16(strip, 12) ==
                              std::vector<u16>({3, 1, 1, 3, 1, 42, 42, 1, 9,
                                                42, 9, 5}),
           "Quad strips become two triangles per quad");
    const auto loop = decode(engines::PrimitiveType::LineLoop, 3, type,
                             primitive, count);
    expect(primitive == engines::PrimitiveType::Lines && count == 6 &&
               read_u16(loop, 6) == std::vector<u16>({3, 1, 1, 42, 42, 3}),
           "Line loops become closed line lists");
    expect(!decode(engines::PrimitiveType::TriangleFan, 2, type, primitive,
                   count)
                .isValid() &&
               count == 0,
           "Degenerate fans draw nothing");

    std::vector<u32> wide = {70000, 1, 2, 70001};
    engines::IndexType wide_type;
    const auto wide_quads = cache.decode(
        commands.get(),
        {.type = engines::IndexType::UInt32,
         .primitive_type = engines::PrimitiveType::Quads,
         .count = 4,
         .mem_range = ztd::Range<uptr>::fromSize(
             reinterpret_cast<uptr>(wide.data()), wide.size() * 4)},
        wide_type, primitive, count);
    std::vector<u32> wide_indices(6);
    std::memcpy(wide_indices.data(),
                reinterpret_cast<const void*>(wide_quads.getPtr()), 24);
    expect(wide_type == engines::IndexType::UInt32 &&
               wide_indices ==
                   std::vector<u32>({70000, 1, 2, 70000, 2, 70001}),
           "U32 quads keep 32-bit indices");
    commands.reset();
    std::cout << "PASS: decoded index buffers follow rewritten guest indices\n";
}

template <typename T>
void checkPrimitiveRestart(engines::IndexType input_type) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    metal::Renderer renderer;
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    auto& cache = renderer.getIndexCache();
    using P = engines::PrimitiveType;
    struct Case {
        P input;
        P output;
        std::vector<u32> expected;
    };
    const Case cases[] = {
        {P::Points, P::Points, {1, 2, 3, 4, 5, 6, 7, 8}},
        {P::Lines, P::Lines, {1, 2, 3, 4, 5, 6, 7, 8}},
        {P::Triangles, P::Triangles, {1, 2, 3, 5, 6, 7}},
        {P::LineStrip, P::Lines, {1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 7, 8}},
        {P::LineLoop, P::Lines,
         {1, 2, 2, 3, 3, 4, 4, 1, 5, 6, 6, 7, 7, 8, 8, 5}},
        {P::TriangleStrip, P::Triangles, {1, 2, 3, 3, 2, 4, 5, 6, 7, 7, 6, 8}},
        {P::TriangleFan, P::Triangles, {1, 2, 3, 1, 3, 4, 5, 6, 7, 5, 7, 8}},
        {P::Polygon, P::Triangles, {1, 2, 3, 1, 3, 4, 5, 6, 7, 5, 7, 8}},
        {P::Quads, P::Triangles, {1, 2, 3, 1, 3, 4, 5, 6, 7, 5, 7, 8}},
        {P::QuadStrip, P::Triangles, {1, 2, 4, 1, 4, 3, 5, 6, 8, 5, 8, 7}},
    };
    std::vector<T> source = {99, 1, 2, 3, 4, 99, 99, 5, 6, 7, 8, 99};
    IndexDescriptor descriptor{
        .type = input_type,
        .primitive_type = P::Points,
        .count = static_cast<u32>(source.size()),
        .mem_range = ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(source.data()), source.size() * sizeof(T)),
        .restart_enabled = true,
        .restart_index = 99};
    engines::IndexType type;
    P primitive;
    u32 count;
    const auto read = [&](const BufferView& view) {
        std::vector<u32> result;
        for (u32 i = 0; i < count; i++) {
            if (type == engines::IndexType::UInt32)
                result.push_back(reinterpret_cast<const u32*>(view.getPtr())[i]);
            else
                result.push_back(reinterpret_cast<const u16*>(view.getPtr())[i]);
        }
        return result;
    };
    const auto decode = [&] {
        return cache.decode(commands.get(), descriptor, type, primitive, count);
    };
    for (const auto& test : cases) {
        descriptor.primitive_type = test.input;
        const auto view = decode();
        expect(primitive == test.output && read(view) == test.expected,
               "Restart separates primitive assembly and resets strip winding");
        expect(decode().getBase() == view.getBase() && count == test.expected.size(),
               "Cached restart decode restores the segmented output count");
    }

    descriptor.primitive_type = P::TriangleStrip;
    descriptor.restart_enabled = false;
    expect(read(decode()).size() == (source.size() - 2) * 3,
           "Disabled custom restart keeps every index");
    descriptor.restart_enabled = true;
    descriptor.restart_index = 98;
    expect(read(decode()).size() == (source.size() - 2) * 3,
           "Changing the restart index invalidates the decoded buffer");
    descriptor.restart_index = 99;
    source.assign(source.size(), 99);
    expect(!decode().isValid() && count == 0,
           "An all-restart draw produces no primitives");

    // The maximum index remains a vertex when restart is disabled.
    source = {1, 2, static_cast<T>(~T{0}), 3, 4};
    descriptor.mem_range = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(source.data()), source.size() * sizeof(T));
    descriptor.count = static_cast<u32>(source.size());
    descriptor.restart_index = static_cast<u32>(source[2]);
    descriptor.restart_enabled = false;
    const auto maximum = static_cast<u32>(source[2]);
    expect(read(decode()) == std::vector<u32>({1, 2, maximum, maximum, 2, 3,
                                             maximum, 3, 4}),
           "Disabled restart preserves maximum indices without Metal strip cuts");
    descriptor.restart_enabled = true;
    const auto native_restart = decode();
    if (sizeof(T) == 1)
        expect(!native_restart.isValid() && count == 0,
               "U8 maximum restart drops incomplete strips on both sides");
    else
        expect(primitive == P::TriangleStrip && count == source.size() &&
                   read(native_restart) == std::vector<u32>({1, 2, maximum, 3, 4}),
               "Fixed U16/U32 restart uses the native Metal strip path");
    descriptor.restart_index = 0xffffffff;
    if (sizeof(T) < sizeof(u32))
        expect(read(decode()).size() == 9,
               "Restart compares the full register without truncating to index width");
}

void testPrimitiveRestart() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    checkPrimitiveRestart<u8>(engines::IndexType::UInt8);
    checkPrimitiveRestart<u16>(engines::IndexType::UInt16);
    checkPrimitiveRestart<u32>(engines::IndexType::UInt32);
    std::cout << "PASS: primitive restart across index widths and topologies\n";
}

void testInlineIndices() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& engine = gpu.get3DEngine();
    engine.method(0xdf, 0); // discard, no shader or guest memory required
    const auto before = PerfStats::instance().draws.load();
    engine.method(0x586, static_cast<u32>(engines::PrimitiveType::Triangles));
    engine.method(0x57a, 70000);
    engine.method(0x57c, 0xabcd1234);
    engine.method(0x4c1, 0x04030201);
    expect(engine.getInlineIndices() ==
               std::vector<u32>({70000, 0x1234, 0xabcd, 1, 2, 3, 4}),
           "Inline indices unpack low components first into 32-bit words");
    expect(PerfStats::instance().draws.load() == before,
           "Inline methods accumulate until END");
    engine.method(0x585, 0);
    expect(engine.getInlineIndices().empty() &&
               PerfStats::instance().draws.load() == before + 1,
           "END submits one indexed draw and consumes inline data");
    engine.method(0x585, 0);
    expect(PerfStats::instance().draws.load() == before + 1,
           "Repeated END does not draw consumed indices again");
    engine.method(0x57a, 42);
    engine.method(0x586, 4);
    expect(engine.getInlineIndices().empty(), "BEGIN resets inline accumulation");
    engine.method(0x591, 1);
    engine.method(0x592, 99);
    expect(engine.getRegs().primitive_restart_enabled == 1 &&
               engine.getRegs().primitive_restart_index == 99,
           "Restart registers match NVIDIA method offsets");
    engine.method(0x35d, 7);
    engine.method(0x4c6, 0);
    engine.method(0x48f, 160);
    expect(PerfStats::instance().draws.load() == before + 1,
           "DRAW_AUTO with zero stride is ignored safely");
    engine.method(0x4c6, 16);
    engine.method(0x48f, 160);
    expect(PerfStats::instance().draws.load() == before + 2 &&
               engine.getRegs().vertex_array_start == 7,
           "DRAW_AUTO submits byte count divided by stride from vertex zero");
    std::cout << "PASS: inline indices, restart registers and DRAW_AUTO fallback\n";
}

void testClippedBlitRegions() {
    engines::Regs2D regs{};
    regs.dst.width = 100;
    regs.dst.height = 100;
    auto& p = regs.pixels_from_memory;
    p.dst_x0 = 10;
    p.dst_y0 = 20;
    p.dst_width = 40;
    p.dst_height = 30;
    p.dudx = {0, 2};
    p.dvdy = {0x80000000u, 0};
    p.src_x0 = {0x40000000u, 5};
    p.src_y0 = {0, 7};
    regs.clip_enabled = 1;
    regs.clip_x = 15;
    regs.clip_y = 22;
    regs.clip_width = 20;
    regs.clip_height = 10;
    auto region = engines::getBlitRegion(regs);
    expect(region && region->dst_x == 15 && region->dst_y == 22 &&
               region->width == 20 && region->height == 10 &&
               region->src_x == 15.25 && region->src_y == 8.0 &&
               region->src_width == 40.0 && region->src_height == 5.0,
           "Clipping adjusts scaled source coordinates without rounding");
    regs.sample_mode.origin = 1;
    region = engines::getBlitRegion(regs);
    expect(region && region->src_x == 14.25 && region->src_y == 8.0,
           "CORNER applies the reference whole-texel derivative correction");
    p.dudx = {0, 1};
    region = engines::getBlitRegion(regs);
    expect(region && region->src_x == 10.25,
           "CORNER keeps one-to-one copies aligned to the same texels");
    regs.sample_mode.origin = 0;
    p.dudx = {0, 0xffffffffu};
    region = engines::getBlitRegion(regs);
    expect(region && region->src_x == 0.25 && region->src_width == -20.0,
           "Clipping preserves mirrored source extents");
    regs.clip_x = 60;
    expect(!engines::getBlitRegion(regs), "Disjoint clip rectangles skip the blit");
    regs.clip_x = 15;
    regs.clip_width = 0;
    expect(!engines::getBlitRegion(regs), "Empty clip rectangles skip the blit");
    regs.clip_enabled = 0;
    p.dst_x0 = 90;
    region = engines::getBlitRegion(regs);
    expect(region && region->width == 10 && region->height == 30,
           "Destination bounds clip copies even when guest clipping is disabled");
    p.dst_x0 = 0xfffffff0u;
    expect(!engines::getBlitRegion(regs), "Overflowing rectangles do not wrap onto the surface");
    std::cout << "PASS: scaled, mirrored and empty 2D clip rectangles\n";
}

// Objects whose descriptors have the same hash stay distinct
struct FakeDescriptor {
    u32 id;
};
class CollidingCache
    : public CacheBase<CollidingCache, u32*, FakeDescriptor, true> {
  public:
    u32 created{0};

    u32* create(const FakeDescriptor& descriptor) {
        created++;
        return new u32(descriptor.id);
    }
    void update(u32*) {}
    void destroy() {}
    static u32 hash(const FakeDescriptor&) { return 7; }
    static bool equals(const FakeDescriptor& a, const FakeDescriptor& b) {
        return a.id == b.id;
    }
    static void destroyElement(u32* value) { delete value; }
};

void testCacheCollisions() {
    CollidingCache cache;
    u32*& first = cache.find({1});
    u32* const first_value = first;
    expect(*cache.find({2}) == 2 && *cache.find({1}) == 1,
           "Colliding descriptors get their own objects");
    for (u32 id = 3; id < 64; id++)
        cache.find({id});
    expect(first == first_value && cache.find({1}) == first_value &&
               cache.created == 63,
           "Entries keep their address when their bucket grows");
    std::cout << "PASS: cache entries with colliding hashes stay distinct\n";
}

void testStorageWritesRefreshTextures() {
    using namespace renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    const auto page_size = static_cast<usize>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<u8*>(mmap(nullptr, page_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    expect(memory != MAP_FAILED, "Storage texture memory allocation");
    std::unique_ptr<u8, std::function<void(u8*)>> backing(
        memory, [page_size](u8* ptr) { munmap(ptr, page_size); });
    metal::Renderer renderer;
    auto* device = renderer.getDevice();
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library = device->newLibrary(NS::String::string(R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void fill(device uint* b [[buffer(0)]],
                         uint t [[thread_position_in_grid]]) {
            b[t] = 0x44332211u;
        })", NS::UTF8StringEncoding), nullptr, &error);
    expect(library != nullptr, "Storage texture kernel compiles");
    NS_STACK_SCOPED auto function = library->newFunction(
        NS::String::string("fill", NS::UTF8StringEncoding));
    NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, "Storage texture pipeline compiles");
    const auto address = reinterpret_cast<uptr>(memory);
    // Cached uploads, tiled layouts and partial overwrites of dirty targets.
    for (u32 variant = 0; variant < 3; ++variant) {
        std::memset(memory, 0x19, page_size);
        auto& cache = renderer.getTextureCache();
        const TextureDescriptor descriptor(address, MTL::TextureType2D,
            TextureFormat::RGBA8Unorm, variant != 1, variant != 1 ? 64 : 0,
            16, 16, 1, 1, 1, 0, 0, 0);
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        cache.invalidateMemory(descriptor.getRange());
        auto* before = cache.find(commands.get(), descriptor, TextureUsage::Read);
        if (variant == 2)
            cache.find(commands.get(), descriptor, TextureUsage::Write);
        const auto written = ztd::Range<uptr>::fromSize(address,
            variant == 2 ? sizeof(u32) : descriptor.size);
        if (variant == 2) {
            auto* source = device->newBuffer(descriptor.size,
                                             MTL::ResourceStorageModeShared);
            std::memset(source->contents(), 0x2a, descriptor.size);
            static_cast<metal::CommandBuffer*>(commands.get())
                ->getBlitCommandEncoder()->copyFromBuffer(source, 0, 64, 1024,
                    MTL::Size(16, 16, 1),
                    static_cast<metal::Texture*>(before->getBase())->getTexture(),
                    0, 0, MTL::Origin(0, 0, 0));
            source->release();
            cache.flushMemory(commands.get(), written);
            expect(memory[0] == 0x2a && memory[4] == 0x19,
                   "Storage preparation reads only its range from the target");
        }
        const auto buffer = renderer.getGuestMemoryBuffer(written);
        expect(buffer.isValid(), "Storage texture memory is GPU accessible");
        auto* encoder = static_cast<metal::CommandBuffer*>(commands.get())
                            ->getComputeCommandEncoder();
        encoder->setComputePipelineState(pipeline);
        encoder->setBuffer(static_cast<metal::Buffer*>(buffer.getBase())->getBuffer(),
                           buffer.getOffset(), 0);
        encoder->dispatchThreads(MTL::Size(written.getSize() / 4, 1, 1),
                                MTL::Size(1, 1, 1));
        cache.synchronizeStorageWrites(commands.get(), {written});
        auto* after = cache.find(commands.get(), descriptor,
            variant == 2 ? TextureUsage::Write : TextureUsage::Read);
        expect(after == before, "Storage writes refresh the existing texture");
        commands.reset();
        const auto pixels = readTexture(device,
            static_cast<metal::Texture*>(after->getBase())->getTexture(), 4);
        expect(pixels[0] == 0x11 && pixels[1] == 0x22 &&
                   pixels[2] == 0x33 && pixels[3] == 0x44,
               "Texture contains completed GPU storage writes");
        if (variant == 2)
            expect(pixels[4] == 0x2a && pixels.back() == 0x2a,
                   "Partial storage write preserves other target pixels");
    }
    std::cout << "PASS: GPU storage writes refresh cached linear/tiled textures\n";
}

void testComputeQmdRead() {
    using engines::ComputeQmd;
    GMmu gmmu(nullptr);
    constexpr uptr BASE = 0x100000;
    ComputeQmd expected{};
    for (u32 i = 0; i < 0x40; i++)
        expected.words[i] = 0x12340000u + i;
    constexpr usize SPLIT = 0x83;
    std::array<u8, SPLIT> first{};
    std::array<u8, sizeof(ComputeQmd) - SPLIT> second{};
    const auto* bytes = reinterpret_cast<const u8*>(&expected);
    std::memcpy(first.data(), bytes, first.size());
    std::memcpy(second.data(), bytes + SPLIT, second.size());

    expect(!ComputeQmd::read(gmmu, BASE), "Unmapped QMD is rejected");
    gmmu.map(BASE, {reinterpret_cast<uptr>(first.data()), first.size()});
    expect(!ComputeQmd::read(gmmu, BASE), "Truncated QMD is rejected");
    gmmu.map(BASE + SPLIT + 1,
             {reinterpret_cast<uptr>(second.data()), second.size()});
    expect(!ComputeQmd::read(gmmu, BASE), "QMD with a mapping hole is rejected");
    gmmu.unmap(BASE + SPLIT + 1);
    gmmu.map(BASE + SPLIT,
             {reinterpret_cast<uptr>(second.data()), second.size()});
    const auto actual = ComputeQmd::read(gmmu, BASE);
    expect(actual && std::memcmp(&*actual, &expected, sizeof(expected)) == 0,
           "QMD spanning separate host mappings preserves every word");
    gmmu.unmap(BASE + SPLIT);
    expect(!ComputeQmd::read(gmmu, BASE), "QMD unmap invalidates the read");
    std::cout << "PASS: compute QMD reads across mappings and rejects holes\n";
}

// Inline-to-memory: byte exact transfers started by LAUNCH_DMA, pitch lines
// and block linear destinations
void testInlineToMemory() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    std::vector<u8> memory(0x10000, 0xee);
    GMmu gmmu(nullptr);
    constexpr uptr BASE = 0x100000;
    gmmu.map(BASE, {reinterpret_cast<uptr>(memory.data()), memory.size()});
    tls_crnt_gmmu = &gmmu;
    std::unique_ptr<renderer::ICommandBuffer> commands(
        gpu.getRenderer().createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    gpu.subchannelMethod(0, 0, 0xa140);
    const auto method = [&](u32 index, u32 arg) {
        gpu.subchannelMethod(0, index, arg);
    };
    const auto setup = [&](u32 length, u32 lines, uptr offset, u32 pitch,
                           bool is_pitch) {
        method(0x60, length);
        method(0x61, lines);
        method(0x62, 0);
        method(0x63, static_cast<u32>(BASE + offset));
        method(0x64, pitch);
        method(0x6c, is_pitch ? 1 : 0);
    };

    // 6 bytes: the second word is partial
    setup(6, 1, 0x10, 0, true);
    method(0x6d, 0x44332211);
    method(0x6d, 0x66665555);
    expect(memory[0x10] == 0x11 && memory[0x13] == 0x44 &&
               memory[0x14] == 0x55 && memory[0x15] == 0x55 &&
               memory[0x16] == 0xee,
           "Inline transfers copy exactly LINE_LENGTH_IN bytes");

    // The next transfer starts clean
    setup(4, 1, 0x100, 0, true);
    method(0x6d, 0xa1a2a3a4);
    u32 word;
    std::memcpy(&word, memory.data() + 0x100, 4);
    expect(word == 0xa1a2a3a4u, "LAUNCH_DMA starts a new transfer");

    // Lines at PITCH_OUT
    setup(4, 3, 0x200, 0x40, true);
    for (u32 line = 0; line < 3; line++)
        method(0x6d, 0x10101010u * (line + 1));
    for (u32 line = 0; line < 3; line++) {
        std::memcpy(&word, memory.data() + 0x200 + line * 0x40, 4);
        expect(word == 0x10101010u * (line + 1),
               "Multi-line transfers use PITCH_OUT");
    }
    expect(memory[0x204] == 0xee, "Bytes between lines are untouched");

    // 2 lines of 24 bytes into a block linear surface (64 bytes wide, 16
    // rows, 2 GOB blocks) at (8, 3)
    constexpr uptr BLOCK_LINEAR = 0x1000;
    method(0x65, 1u << 4);
    method(0x66, 64);
    method(0x67, 16);
    method(0x68, 1);
    method(0x69, 0);
    method(0x6a, 8);
    method(0x6b, 3);
    setup(24, 2, BLOCK_LINEAR, 0, false);
    for (u32 i = 0; i < 12; i++) {
        const u32 b = i * 4;
        method(0x6d, b | ((b + 1) << 8) | ((b + 2) << 16) | ((b + 3) << 24));
    }
    for (u32 line = 0; line < 2; line++) {
        for (u32 x = 0; x < 24; x++) {
            const auto offset =
                getBlockLinearOffset(8 + x, 3 + line, 0, 64, 16, 1, 0);
            expect(memory[BLOCK_LINEAR + offset] == line * 24 + x,
                   "Block linear transfers swizzle each byte");
        }
    }
    expect(memory[BLOCK_LINEAR + getBlockLinearOffset(7, 3, 0, 64, 16, 1, 0)] ==
               0xee,
           "Block linear transfers leave the rest of the surface");

    commands.reset();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: inline-to-memory byte counts, launches, pitch lines "
                 "and block linear destinations\n";
}

// DMA: block linear to block linear sub rectangles, semaphore releases and
// launches without a transfer
void testDmaCopies() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    std::vector<u8> memory(0x10000, 0xee);
    GMmu gmmu(nullptr);
    constexpr uptr BASE = 0x200000;
    gmmu.map(BASE, {reinterpret_cast<uptr>(memory.data()), memory.size()});
    tls_crnt_gmmu = &gmmu;
    std::unique_ptr<renderer::ICommandBuffer> commands(
        gpu.getRenderer().createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    gpu.subchannelMethod(0, 0, 0xb0b5);
    const auto method = [&](u32 index, u32 arg) {
        gpu.subchannelMethod(0, index, arg);
    };

    // Source: 64 x 16 bytes, 2 GOB blocks, byte (x, y) = x + y * 3
    constexpr uptr SOURCE = 0x0, TARGET = 0x4000;
    for (u32 y = 0; y < 16; y++)
        for (u32 x = 0; x < 64; x++)
            memory[SOURCE + getBlockLinearOffset(x, y, 0, 64, 16, 1, 0)] =
                static_cast<u8>(x + y * 3);
    // Target: 128 x 32 bytes, 4 GOB blocks
    method(0x100, 0);
    method(0x101, static_cast<u32>(BASE + SOURCE));
    method(0x102, 0);
    method(0x103, static_cast<u32>(BASE + TARGET));
    method(0x106, 24); // line length
    method(0x107, 4);  // lines
    // Destination info (0x1c3...): block size, stride, height, depth, layer,
    // origin (x | y << 16)
    method(0x1c3, 2u << 4);
    method(0x1c4, 128);
    method(0x1c5, 32);
    method(0x1c6, 1);
    method(0x1c7, 0);
    method(0x1c8, 16u | (5u << 16));
    // Source info (0x1ca...)
    method(0x1ca, 1u << 4);
    method(0x1cb, 64);
    method(0x1cc, 16);
    method(0x1cd, 1);
    method(0x1ce, 0);
    method(0x1cf, 8u | (2u << 16));
    // Pipelined transfer, block linear (0) to block linear, multi line
    method(0xc0, 1u | (1u << 9));
    for (u32 line = 0; line < 4; line++) {
        for (u32 x = 0; x < 24; x++) {
            const u8 expected = static_cast<u8>((8 + x) + (2 + line) * 3);
            expect(memory[TARGET + getBlockLinearOffset(16 + x, 5 + line, 0, 128,
                                                        32, 2, 0)] == expected,
                   "Block linear to block linear copies a sub rectangle");
        }
    }
    expect(memory[TARGET + getBlockLinearOffset(15, 5, 0, 128, 32, 2, 0)] ==
               0xee,
           "Block linear copies leave the rest of the target");

    // Launch without a transfer: only the semaphore (one word, then four)
    constexpr uptr SEMAPHORE = 0x8000;
    method(0x90, 0);
    method(0x91, static_cast<u32>(BASE + SEMAPHORE));
    method(0x92, 0xcafe1234u);
    method(0xc0, 1u << 3);
    u32 word;
    std::memcpy(&word, memory.data() + SEMAPHORE, 4);
    expect(word == 0xcafe1234u, "DMA releases a one word semaphore");
    expect(memory[SEMAPHORE + 4] == 0xee, "One word releases write one word");
    method(0x92, 0x1111u);
    method(0xc0, 2u << 3);
    u64 timestamp;
    std::memcpy(&word, memory.data() + SEMAPHORE, 4);
    std::memcpy(&timestamp, memory.data() + SEMAPHORE + 8, 8);
    expect(word == 0x1111u && timestamp != 0,
           "DMA releases a four word semaphore with a timestamp");

    commands.reset();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: DMA block linear copies, semaphores and launches "
                 "without transfers\n";
}

// 2D fixed point coordinates are signed 32.32
void testTwoDFixedPoint() {
    using engines::FixedPoint;
    const auto value = [](u32 integer, u32 fractional) {
        return static_cast<f64>(FixedPoint{fractional, integer});
    };
    if (value(3, 0x80000000u) != 3.5 || value(0xfffffffeu, 0) != -2.0 ||
        value(0xffffffffu, 0x80000000u) != -0.5)
        throw std::runtime_error("2D fixed point decoding");
    std::cout << "PASS: 2D signed fixed point\n";
}

// Channel (puller) methods: semaphores in guest memory, and fences reached
// when the host finished a submission
void testChannelMethodsAndFences() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    std::vector<u8> memory(0x1000, 0xee);
    GMmu gmmu(nullptr);
    constexpr uptr BASE = 0x300000;
    gmmu.map(BASE, {reinterpret_cast<uptr>(memory.data()), memory.size()});
    tls_crnt_gmmu = &gmmu;
    const auto method = [&](u32 index, u32 arg) {
        gpu.subchannelMethod(0, index, arg);
    };
    const auto set_address = [&](uptr offset) {
        method(0x4, 0);
        method(0x5, static_cast<u32>(BASE + offset));
    };

    // Release (4 bytes) and long release (payload and timestamp)
    set_address(0x10);
    method(0x1b, 0xabcd0001u);
    u32 word;
    std::memcpy(&word, memory.data() + 0x10, 4);
    expect(word == 0xabcd0001u && memory[0x14] == 0xee,
           "SEMAPHORE_RELEASE writes one word");
    set_address(0x20);
    method(0x6, 0x7777u);
    method(0x7, 0x2);
    u64 timestamp;
    std::memcpy(&word, memory.data() + 0x20, 4);
    std::memcpy(&timestamp, memory.data() + 0x28, 8);
    expect(word == 0x7777u && timestamp != 0,
           "A long release writes the payload and a timestamp");

    // Acquire: satisfied at once, or held beyond the former 100 ms timeout.
    set_address(0x10);
    const auto begin = std::chrono::steady_clock::now();
    method(0x1a, 0xabcd0001u);
    expect(std::chrono::steady_clock::now() - begin <
               std::chrono::milliseconds(50),
           "An acquired semaphore that holds the value does not wait");
    std::thread delayed_release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        const u32 value = 0x12345678u;
        std::atomic_ref<u32>(*reinterpret_cast<u32*>(memory.data() + 0x10))
            .store(value, std::memory_order_release);
    });
    method(0x1a, 0x12345678u);
    const auto waited = std::chrono::steady_clock::now() - begin;
    delayed_release.join();
    expect(waited >= std::chrono::milliseconds(150),
           "An acquire does not bypass a pending dependency after 100 ms");

    // Another thread releases an awaited semaphore
    set_address(0x30);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const u32 value = 5;
        std::atomic_ref<u32>(*reinterpret_cast<u32*>(memory.data() + 0x30))
            .store(value, std::memory_order_release);
    });
    method(0x6, 5);
    method(0x7, 0x1);
    releaser.join();

    // The queued GPU write would overwrite an early CPU semaphore release.
    NS_STACK_SCOPED auto gpu_memory = static_cast<renderer::metal::Renderer&>(
        gpu.getRenderer()).getDevice()->newBuffer(
        4096, MTL::ResourceStorageModeShared);
    auto* gpu_bytes = static_cast<u8*>(gpu_memory->contents());
    constexpr uptr GPU_MEMORY = BASE + 0x10000;
    gmmu.map(GPU_MEMORY, {reinterpret_cast<uptr>(gpu_bytes), 4096});
    std::unique_ptr<renderer::ICommandBuffer> commands(
        gpu.getRenderer().createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    const auto queue_fill = [&](u8 value) {
        static_cast<renderer::metal::CommandBuffer*>(commands.get())
            ->getBlitCommandEncoder()->fillBuffer(gpu_memory, NS::Range(0, 16), value);
    };
    method(0x5, static_cast<u32>(GPU_MEMORY));
    queue_fill(0x4a);
    method(0x1b, 0x11223344);
    gpu.getRenderer().debugSyncCommandBuffer(commands.get());
    std::memcpy(&word, gpu_bytes, 4);
    expect(word == 0x11223344 && gpu_bytes[4] == 0x4a,
           "Legacy release follows prior GPU writes and touches only four bytes");
    queue_fill(0x6b);
    method(0x6, 0x55667788);
    method(0x7, 0x2 | (1u << 24));
    gpu.getRenderer().debugSyncCommandBuffer(commands.get());
    std::memcpy(&word, gpu_bytes, 4);
    expect(word == 0x55667788 && gpu_bytes[4] == 0x6b && gpu_bytes[8] == 0x6b,
           "Four-byte trigger release waits and leaves timestamp bytes untouched");
    queue_fill(0x7c);
    method(0x7, 0x2);
    gpu.getRenderer().debugSyncCommandBuffer(commands.get());
    std::memcpy(&word, gpu_bytes, 4);
    std::memcpy(&timestamp, gpu_bytes + 8, 8);
    expect(word == 0x55667788 && timestamp != 0x7c7c7c7c7c7c7c7cull,
           "Long release publishes its payload and timestamp after GPU writes");
    queue_fill(0x3d);
    method(0x7, 0x2 | (1u << 20) | (1u << 24));
    expect(static_cast<renderer::metal::CommandBuffer*>(commands.get())
               ->getCommandBuffer()->status() == MTL::CommandBufferStatusNotEnqueued,
           "WFI-disabled release does not submit the current command buffer");
    method(0x1e, 0);
    expect(gpu_bytes[0] == 0x3d && gpu_bytes[15] == 0x3d,
           "WAIT_FOR_IDLE submits and completes prior work");
    queue_fill(0x05);
    method(0x1a, 0x05050505);
    std::memcpy(&word, gpu_bytes, 4);
    expect(word == 0x05050505,
           "Acquire submits earlier GPU work that produces its value");
    tls_crnt_command_buffer = nullptr;
    commands.reset();
    gmmu.unmap(GPU_MEMORY);

    // Fences: reached once the work of the submission finished
    setenv("HYDRA_GPU_FENCES", "1", 1);
    Fence fence{};
    expect(gpu.reserveFence(fence) && fence.id != 0 && fence.value == 1,
           "A fence reserves a syncpoint value");
    Fence second{};
    expect(gpu.reserveFence(second) && second.id == fence.id &&
               second.value == 2,
           "Fences count up on the same syncpoint");
    auto& syncpoints = gpu.getVideoSyncpoints();
    expect(!syncpoints.waitFor(fence.id, fence.value,
                               std::chrono::milliseconds(10)),
           "A fence is not reached before its work finished");
    std::promise<void> done;
    gpu.getPfifo().submitEntries(gmmu, {}, GpfifoFlags::FenceGet,
                                 [&, signal = gpu.makeFenceSignal(fence)] {
                                     signal();
                                     done.set_value();
                                 });
    expect(done.get_future().wait_for(std::chrono::seconds(5)) ==
               std::future_status::ready,
           "The host completes a submission");
    expect(syncpoints.waitFor(fence.id, fence.value,
                              std::chrono::milliseconds(1000)) &&
               !syncpoints.waitFor(second.id, second.value,
                                   std::chrono::milliseconds(10)),
           "A fence is reached by its own submission only");

    // FENCE_WAIT: the GPU does not start a submission before the fence it
    // waits for is reached
    Fence awaited{};
    expect(gpu.reserveFence(awaited) && awaited.id == fence.id &&
               awaited.value == 3,
           "A third fence reserves the next value");
    std::promise<void> waited_ran;
    gpu.getPfifo().submitEntries(gmmu, {}, GpfifoFlags::FenceWait,
                                 [&] { waited_ran.set_value(); }, awaited);
    auto waited_future = waited_ran.get_future();
    expect(waited_future.wait_for(std::chrono::milliseconds(1200)) !=
               std::future_status::ready,
           "A submission waits for its fence before it runs");
    syncpoints.complete(awaited.id, 2);
    expect(waited_future.wait_for(std::chrono::seconds(5)) ==
               std::future_status::ready,
           "A submission runs once its fence is reached");

    commands.reset(gpu.getRenderer().createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    queue_fill(0x09);
    method(0x1d, (awaited.id << 8) | 1);
    expect(gpu_bytes[0] == 0x09 &&
               syncpoints.read(awaited.id) == awaited.value + 1,
           "Emulated syncpoint increment follows prior GPU work");
    const auto engine_syncpoint = syncpoints.allocate();
    expect(engine_syncpoint.has_value(), "Allocate a 3D syncpoint");
    u32 expected_count = 0;
    for (u32 flags : {0u, 1u << 16, 1u << 20, (1u << 16) | (1u << 20)}) {
        ++expected_count;
        const auto fill = static_cast<u8>(0x20u + expected_count);
        queue_fill(fill);
        gpu.get3DEngine().method(0xb2, *engine_syncpoint | flags);
        expect(syncpoints.read(*engine_syncpoint) == expected_count &&
                   gpu_bytes[0] == fill && gpu_bytes[15] == fill,
               "3D stream-out/ROP syncpoint increments follow real GPU writes with either CLEAN_L2 setting");
    }
    tls_crnt_command_buffer = nullptr;
    commands.reset();

    // An unresolved fence must remain cancellable when the emulator stops.
    std::atomic<bool> bypassed{false};
    gpu.getPfifo().submitEntries(gmmu, {}, GpfifoFlags::FenceWait,
        [&] { bypassed = true; }, {awaited.id, awaited.value + 2});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto stopping = std::chrono::steady_clock::now();
    gpu.getPfifo().shutdown();
    expect(!bypassed && std::chrono::steady_clock::now() - stopping <
               std::chrono::seconds(1),
           "Shutdown cancels a pending fence without running its submission");

    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: channel semaphores and GPU fences\n";
}

} // namespace

void runBlit2DTests();

int main() {
    try {
        runBlit2DTests();
        testMacroAlu();
        testMacroBitfields();
        testMacroControlFlow();
        testMacroParametersAndNesting();
        testMacroResultOperations();
        testRendererClears();
        testGuestMemoryBuffer();
        testOcclusionReports();
        testReportSemaphoreReductionAndAcquire();
        testConditionalRendering();
        testIndexDecoding();
        testPrimitiveRestart();
        testInlineIndices();
        testClippedBlitRegions();
        testCacheCollisions();
        testComputeQmdRead();
        testStorageWritesRefreshTextures();
        testInlineToMemory();
        testDmaCopies();
        testChannelMethodsAndFences();
        testTwoDFixedPoint();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

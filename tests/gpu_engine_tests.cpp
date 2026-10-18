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

void testConstBufferRemapping() {
    using namespace hydra::hw::tegra_x1::gpu::engines;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& engine = gpu.get3DEngine();
    auto& renderer = gpu.getRenderer();
    GMmu gmmu(nullptr);
    GMmu other(nullptr);
    std::array<u32, 16> original{}, replacement{};
    original.fill(0x11223344);
    replacement.fill(0xaabbccdd);
    const uptr old_ptr = reinterpret_cast<uptr>(original.data());
    const uptr new_ptr = reinterpret_cast<uptr>(replacement.data());
    constexpr u64 ADDRESS = 0x123400000ull;
    constexpr u32 SLOT = 3;
    constexpr u32 STAGE = static_cast<u32>(ShaderStage::Fragment) - 1;
    constexpr u64 OFFSET = 16;
    constexpr u64 SIZE = 32;
    gmmu.map(ADDRESS, {old_ptr, sizeof(original), 0x80000000});
    tls_crnt_gmmu = &gmmu;
    std::unique_ptr<renderer::ICommandBuffer> commands(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    struct ResetContext {
        ~ResetContext() {
            tls_crnt_command_buffer = nullptr;
            tls_crnt_gmmu = nullptr;
        }
    } reset_context;
    const auto bind = [&](u64 address, u32 size, u32 stage = STAGE) {
        engine.method(0x8e0, size);
        engine.method(0x8e1, static_cast<u32>(address >> 32));
        engine.method(0x8e2, static_cast<u32>(address));
        engine.method(0x904 + stage * 8, 1 | (SLOT << 4));
    };
    const auto check_range = [&](uptr ptr) {
        const auto range = engine.getConstBuffer(STAGE, SLOT);
        expect(range.getBegin() == ptr && range.getSize() == SIZE,
               "Constant buffer resolves the complete current GPU mapping");
    };
    bind(ADDRESS + OFFSET, SIZE);
    check_range(old_ptr + OFFSET);
    const auto before = engine.getConstBufferUpload(STAGE, SLOT);
    expect(*reinterpret_cast<const u32*>(before.getPtr()) == original[4],
           "Initial constant buffer upload reads original backing");

    gmmu.map(ADDRESS, {new_ptr, sizeof(replacement)});
    check_range(new_ptr + OFFSET);
    const auto after = engine.getConstBufferUpload(STAGE, SLOT);
    expect(*reinterpret_cast<const u32*>(after.getPtr()) == replacement[4] &&
               *reinterpret_cast<const u32*>(before.getPtr()) == original[4],
           "Remap without bind uploads new data and retains the previous snapshot");

    gmmu.unmap(ADDRESS);
    expect(engine.getConstBuffer(STAGE, SLOT).getSize() == 0,
           "Unmapped constant buffer does not retain a stale host pointer");
    gmmu.map(ADDRESS, {old_ptr, sizeof(original), 0x80000000});
    gmmu.rebaseHostRange(old_ptr, old_ptr + sizeof(original), new_ptr,
                        sizeof(replacement));
    check_range(new_ptr + OFFSET);

    other.map(ADDRESS, {old_ptr, sizeof(original)});
    tls_crnt_gmmu = &other;
    check_range(old_ptr + OFFSET);
    tls_crnt_gmmu = &gmmu;
    check_range(new_ptr + OFFSET);
    expect(engine.getConstBuffer(0, SLOT).getSize() == 0 &&
               engine.getConstBuffer(STAGE, SLOT + 1).getSize() == 0,
           "Constant buffer bindings remain isolated by stage and slot");

    // Both contiguous and scattered mappings preserve the complete binding.
    gmmu.unmap(ADDRESS);
    gmmu.map(ADDRESS, {new_ptr, 32});
    gmmu.map(ADDRESS + 32, {new_ptr + 32, 32});
    check_range(new_ptr + OFFSET);
    gmmu.map(ADDRESS + 32, {old_ptr, 32});
    const auto scattered = engine.getConstBuffer(STAGE, SLOT);
    expect(scattered.getSize() == SIZE &&
               std::memcmp(reinterpret_cast<const void*>(scattered.getBegin()),
                           replacement.data() + 4, 16) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(scattered.getBegin() + 16),
                           original.data(), 16) == 0,
           "Scattered constant buffers gather bytes in GPU address order");
    const auto scattered_before = engine.getConstBufferUpload(STAGE, SLOT);
    const auto reused = engine.getConstBufferUpload(STAGE, SLOT);
    expect(reused.getBase() == scattered_before.getBase(),
           "Unchanged gathered bytes reuse the immutable upload");
    original[0] = 0x13579bdf;
    const auto scattered_after = engine.getConstBufferUpload(STAGE, SLOT);
    expect(scattered_after.getBase() != scattered_before.getBase() &&
               reinterpret_cast<const u32*>(scattered_after.getPtr())[4] == original[0] &&
               reinterpret_cast<const u32*>(scattered_before.getPtr())[4] == 0x11223344,
           "CPU writes refresh gathered uploads without mutating earlier snapshots");

    auto* device = static_cast<renderer::metal::Renderer&>(renderer).getDevice();
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library = device->newLibrary(
        renderer::metal::toNsString(R"(
#include <metal_stdlib>
using namespace metal;
kernel void copy_words(constant uint* input [[buffer(0)]],
                       device uint* output [[buffer(1)]],
                       uint tid [[thread_position_in_grid]]) {
    output[tid] = input[tid];
}
)"), nullptr, &error);
    expect(library != nullptr, "Compile gathered uniform readback shader");
    NS_STACK_SCOPED auto function =
        library->newFunction(renderer::metal::toNsString("copy_words"));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, "Create gathered uniform readback pipeline");
    NS_STACK_SCOPED auto output = device->newBuffer(64, MTL::ResourceStorageModeShared);
    auto* metal_commands = static_cast<renderer::metal::CommandBuffer*>(commands.get());
    auto* encoder = metal_commands->getComputeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    const auto encode = [&](const renderer::BufferView& buffer, u64 offset) {
        auto* native = static_cast<renderer::metal::Buffer*>(buffer.getBase())->getBuffer();
        encoder->setBuffer(native, buffer.getOffset(), 0);
        encoder->setBuffer(output, offset, 1);
        encoder->dispatchThreads(MTL::Size(8, 1, 1), MTL::Size(8, 1, 1));
    };
    encode(scattered_before, 0);
    encode(scattered_after, 32);
    auto* native = metal_commands->getCommandBuffer()->retain();
    tls_crnt_command_buffer = nullptr;
    commands.reset();
    native->waitUntilCompleted();
    const bool completed = native->status() == MTL::CommandBufferStatusCompleted;
    native->release();
    expect(completed, "GPU reads gathered uniform snapshots successfully");
    const auto* words = static_cast<const u32*>(output->contents());
    for (u32 i = 0; i < 16; ++i)
        expect(words[i] == (i % 8 < 4 ? 0xaabbccdd
                           : i == 12 ? 0x13579bdf : 0x11223344),
               "GPU sees both sides of each immutable gathered constant buffer");
    commands.reset(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    gmmu.unmap(ADDRESS + 32);
    expect(engine.getConstBuffer(STAGE, SLOT).getSize() == 0,
           "Partially mapped constant buffers are unavailable");
    gmmu.map(ADDRESS + 32, {new_ptr + 32, 32});
    check_range(new_ptr + OFFSET);
    engine.method(0x904 + STAGE * 8, SLOT << 4);
    expect(engine.getConstBuffer(STAGE, SLOT).getSize() == 0,
           "Disable clears the GPU binding");
    bind(ADDRESS, 0);
    expect(engine.getConstBuffer(STAGE, SLOT).getSize() == 0,
           "Empty constant buffers do not resolve a host range");
    bind(ADDRESS + OFFSET, SIZE | (1u << 20));
    check_range(new_ptr + OFFSET);
    tls_crnt_gmmu = nullptr;
    tls_crnt_command_buffer = nullptr;
    commands.reset();
    std::cout << "PASS: constant buffers follow GPU remap, unmap and heap rebase without rebinding\n";
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
// Regression for the captured light-shaft occlusion draw: explicit alpha
// REVERSE_SUBTRACT must not turn into color ADD at pipeline construction.
void testExplicitAlphaBlendEquation() {
    using namespace hw::tegra_x1::gpu::engines;
    Regs3D regs{};
    for (u32 flag : {0u, 1u, 2u}) {
        regs.blend_state = {.separate_for_alpha = flag, .rgb_op = 1,
                            .src_rgb_factor = 2, .dst_rgb_factor = 6,
                            .alpha_op = 3, .src_alpha_factor = 2,
                            .dst_alpha_factor = 2};
        const auto equation = getBlendEquation(regs.blend_state);
        expect(equation.rgb_op == 1 && equation.src_rgb_factor == 2 &&
                   equation.dst_rgb_factor == 6 && equation.alpha_op == 3 &&
                   equation.src_alpha_factor == 2 && equation.dst_alpha_factor == 2,
               "Captured alpha occlusion equation survives pipeline resolution");
        regs.independent_blend_state[3] = {.separate_for_alpha = flag, .rgb_op = 1,
                                           .src_rgb_factor = 1, .dst_rgb_factor = 1,
                                           .alpha_op = 5, .src_alpha_factor = 5,
                                           .dst_alpha_factor = 7};
        const auto independent = getBlendEquation(regs.independent_blend_state[3]);
        expect(independent.alpha_op == 5 && independent.src_alpha_factor == 5 &&
                   independent.dst_alpha_factor == 7 && independent.rgb_op == 1,
               "Independent targets preserve explicit alpha factors and operation");
    }
    std::cout << "PASS: explicit color and alpha blend equations are preserved\n";
}

// The register file mirrors the method space: a field's byte offset in Regs3D
// is the method's byte offset in NVIDIA's clb197.h (checked against the header
// text, https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h).
void testRegisterLayout() {
    using namespace hw::tegra_x1::gpu::engines;
    usize checked = 0;
    const auto at = [&](usize offset, usize header, const char* name) {
        if (offset != header)
            throw std::runtime_error(
                std::string("Regs3D offset differs from clb197.h: ") + name);
        ++checked;
    };
#define HYDRA_REG(member, header_offset, name) \
    at(offsetof(Regs3D, member), header_offset, name)
    HYDRA_REG(color_targets, 0x800, "SET_COLOR_TARGET_A(0)");
    HYDRA_REG(viewport_transforms, 0xa00, "SET_VIEWPORT_SCALE_X(0)");
    HYDRA_REG(viewports, 0xc00, "SET_VIEWPORT_CLIP_HORIZONTAL(0)");
    HYDRA_REG(window_rects, 0xd00, "SET_WINDOW_CLIP_HORIZONTAL(0)");
    HYDRA_REG(vertex_array_start, 0xd74, "SET_VERTEX_ARRAY_START");
    HYDRA_REG(viewport_z_clip, 0xd7c, "SET_VIEWPORT_Z_CLIP");
    HYDRA_REG(clear_color, 0xd80, "SET_COLOR_CLEAR_VALUE(0)");
    HYDRA_REG(clear_depth, 0xd90, "SET_Z_CLEAR_VALUE");
    HYDRA_REG(clear_stencil, 0xda0, "SET_STENCIL_CLEAR_VALUE");
    HYDRA_REG(scissors, 0xe00, "SET_SCISSOR_ENABLE(0)");
    HYDRA_REG(depth_target_addr, 0xfe0, "SET_ZT_A");
    HYDRA_REG(depth_target_layer_stride, 0xff0, "SET_ZT_ARRAY_PITCH");
    HYDRA_REG(screen_scissor, 0xff4, "SET_SURFACE_CLIP_HORIZONTAL");
    HYDRA_REG(vertex_attrib_states, 0x1160, "SET_VERTEX_ATTRIBUTE_A(0)");
    HYDRA_REG(color_target_control, 0x121c, "SET_CT_SELECT");
    HYDRA_REG(depth_target_width, 0x1228, "SET_ZT_SIZE_A");
    HYDRA_REG(depth_target_height, 0x122c, "SET_ZT_SIZE_B");
    HYDRA_REG(depth_target_array_mode, 0x1230, "SET_ZT_SIZE_C");
    HYDRA_REG(depth_test_enabled, 0x12cc, "SET_DEPTH_TEST");
    HYDRA_REG(independent_blend_enabled, 0x12e4, "SET_BLEND_STATE_PER_TARGET");
    HYDRA_REG(depth_write_enabled, 0x12e8, "SET_DEPTH_WRITE");
    HYDRA_REG(depth_compare_op, 0x130c, "SET_DEPTH_FUNC");
    HYDRA_REG(blend_constant, 0x131c, "SET_BLEND_CONST_RED");
    HYDRA_REG(blend_state.separate_for_alpha, 0x133c, "SET_BLEND_SEPARATE_FOR_ALPHA");
    HYDRA_REG(blend_state.rgb_op, 0x1340, "SET_BLEND_COLOR_OP");
    HYDRA_REG(blend_state.src_rgb_factor, 0x1344, "SET_BLEND_COLOR_SOURCE_COEFF");
    HYDRA_REG(blend_state.dst_rgb_factor, 0x1348, "SET_BLEND_COLOR_DEST_COEFF");
    HYDRA_REG(blend_state.alpha_op, 0x134c, "SET_BLEND_ALPHA_OP");
    HYDRA_REG(blend_state.src_alpha_factor, 0x1350, "SET_BLEND_ALPHA_SOURCE_COEFF");
    HYDRA_REG(blend_state.global_color_key, 0x1354, "SET_GLOBAL_COLOR_KEY");
    HYDRA_REG(blend_state.dst_alpha_factor, 0x1358, "SET_BLEND_ALPHA_DEST_COEFF");
    HYDRA_REG(single_rop_control, 0x135c, "SET_SINGLE_ROP_CONTROL");
    HYDRA_REG(color_blend_enabled, 0x1360, "SET_BLEND(0)");
    HYDRA_REG(window_origin_flags, 0x13ac, "SET_WINDOW_ORIGIN");
    HYDRA_REG(base_vertex, 0x1434, "SET_GLOBAL_BASE_VERTEX_INDEX");
    HYDRA_REG(base_instance, 0x1438, "SET_GLOBAL_BASE_INSTANCE_INDEX");
    HYDRA_REG(tex_header_pool_max_index, 0x157c, "SET_TEX_HEADER_POOL_C");
    HYDRA_REG(shader_program_region, 0x1608, "SET_PROGRAM_REGION_A");
    HYDRA_REG(end, 0x1614, "END");
    HYDRA_REG(begin, 0x1618, "BEGIN");
    HYDRA_REG(primitive_restart_enabled, 0x1644, "SET_DA_PRIMITIVE_RESTART");
    HYDRA_REG(primitive_restart_index, 0x1648, "SET_DA_PRIMITIVE_RESTART_INDEX");
    HYDRA_REG(index_buffer_addr, 0x17c8, "SET_INDEX_BUFFER_A");
    HYDRA_REG(index_buffer_limit_addr, 0x17d0, "SET_INDEX_BUFFER_C");
    HYDRA_REG(index_type, 0x17d8, "SET_INDEX_BUFFER_E");
    HYDRA_REG(vertex_elements_start, 0x17dc, "SET_INDEX_BUFFER_F");
    HYDRA_REG(is_vertex_array_per_instance, 0x1880, "SET_VERTEX_STREAM_INSTANCE_A(0)");
    HYDRA_REG(cull_face_enabled, 0x1918, "OGL_SET_CULL");
    HYDRA_REG(front_face_winding, 0x191c, "OGL_SET_FRONT_FACE");
    HYDRA_REG(cull_face_mode, 0x1920, "OGL_SET_CULL_FACE");
    HYDRA_REG(viewport_transform_enabled, 0x192c, "SET_VIEWPORT_SCALE_OFFSET");
    HYDRA_REG(color_write_masks, 0x1a00, "SET_CT_WRITE(0)");
    HYDRA_REG(report_semaphore_addr, 0x1b00, "SET_REPORT_SEMAPHORE_A");
    HYDRA_REG(report_semaphore_payload, 0x1b08, "SET_REPORT_SEMAPHORE_C");
    HYDRA_REG(vertex_arrays, 0x1c00, "SET_VERTEX_STREAM_A_FORMAT(0)");
    HYDRA_REG(independent_blend_state, 0x1e00, "SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(0)");
    HYDRA_REG(vertex_array_limits, 0x1f00, "SET_VERTEX_STREAM_LIMIT_A_A(0)");
    HYDRA_REG(shader_programs, 0x2000, "SET_PIPELINE_SHADER(0)");
    HYDRA_REG(const_buffer_selector_size, 0x2380, "SET_CONSTANT_BUFFER_SELECTOR_A");
    HYDRA_REG(load_const_buffer_offset, 0x238c, "LOAD_CONSTANT_BUFFER_OFFSET");
    HYDRA_REG(bindless_texture_const_buffer_slot, 0x2608, "SET_BINDLESS_TEXTURE");
#undef HYDRA_REG
    // Strides of the register arrays
    const auto stride = [&](usize size, usize header, const char* name) {
        if (size != header)
            throw std::runtime_error(
                std::string("Regs3D array stride differs from clb197.h: ") + name);
    };
    stride(sizeof(Regs3D::color_targets[0]), 64, "SET_COLOR_TARGET_A(j)");
    stride(sizeof(Regs3D::viewport_transforms[0]), 32, "SET_VIEWPORT_SCALE_X(j)");
    stride(sizeof(Regs3D::viewports[0]), 16, "SET_VIEWPORT_CLIP_HORIZONTAL(j)");
    stride(sizeof(Regs3D::window_rects[0]), 8, "SET_WINDOW_CLIP_HORIZONTAL(j)");
    stride(sizeof(Regs3D::scissors[0]), 16, "SET_SCISSOR_ENABLE(j)");
    stride(sizeof(Regs3D::vertex_attrib_states[0]), 4, "SET_VERTEX_ATTRIBUTE_A(i)");
    stride(sizeof(Regs3D::vertex_arrays[0]), 16, "SET_VERTEX_STREAM_A_FORMAT(j)");
    stride(sizeof(Regs3D::independent_blend_state[0]), 32,
           "SET_BLEND_PER_TARGET_*(j)");
    stride(sizeof(Regs3D::vertex_array_limits[0]), 8, "SET_VERTEX_STREAM_LIMIT_A_A(j)");
    stride(sizeof(Regs3D::shader_programs[0]), 64, "SET_PIPELINE_SHADER(j)");
    std::cout << "PASS: Regs3D layout matches clb197.h (" << checked
              << " registers, 10 array strides)\n";
}

// Every field of the guest texture image/sampler descriptors sits at the bit
// positions of NVIDIA's clb197tex.h (TEXHEAD_BL_* for the block-linear TIC,
// TEXSAMP0-7 for the TSC). Each field is set alone and all others must read
// zero, so overlapping or misplaced fields fail.
template <typename Descriptor, usize WordCount>
struct DescriptorField {
    const char* name;
    u32 lo;
    u32 hi;
    std::function<u64(const Descriptor&)> get;
};

template <typename Descriptor, usize WordCount>
void checkDescriptorFields(
    const char* what,
    const std::vector<DescriptorField<Descriptor, WordCount>>& fields) {
    static_assert(sizeof(Descriptor) == WordCount * sizeof(u32));
    const auto build = [&](const std::vector<std::pair<u32, u32>>& ranges,
                           const std::vector<u64>& values) {
        std::array<u32, WordCount> words{};
        for (usize f = 0; f < ranges.size(); f++)
            for (u32 bit = ranges[f].first; bit <= ranges[f].second; bit++)
                if (((values[f] >> (bit - ranges[f].first)) & 1u) != 0)
                    words[bit / 32] |= 1u << (bit % 32);
        Descriptor descriptor;
        std::memcpy(&descriptor, words.data(), sizeof(descriptor));
        return descriptor;
    };
    for (usize f = 0; f < fields.size(); f++) {
        const u32 width = fields[f].hi - fields[f].lo + 1;
        const u64 all_ones = width == 64 ? ~0ull : (1ull << width) - 1;
        // Alternating pattern as well as all ones, so a shifted field is caught
        for (const u64 value : {all_ones, 0x5555555555555555ull & all_ones}) {
            std::vector<u64> values(fields.size(), 0);
            values[f] = value;
            std::vector<std::pair<u32, u32>> ranges;
            for (const auto& field : fields)
                ranges.push_back({field.lo, field.hi});
            const auto descriptor = build(ranges, values);
            for (usize g = 0; g < fields.size(); g++) {
                const u64 expected = g == f ? value : 0;
                if (fields[g].get(descriptor) != expected)
                    throw std::runtime_error(
                        std::string(what) + ": field " + fields[g].name +
                        (g == f ? " does not read back at its header bits"
                                : " overlaps ") +
                        (g == f ? "" : fields[f].name));
            }
        }
    }
}

void testTextureDescriptorLayout() {
    using namespace hw::tegra_x1::gpu::engines;
#define TIC(name, lo, hi, expr) \
    DescriptorField<TextureImageControl, 8>{name, lo, hi, \
        [](const TextureImageControl& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<TextureImageControl, 8>("TIC (TEXHEAD_BL)", {
        TIC("components", 0, 6, d.format_word.image_format),
        TIC("r_data_type", 7, 9, d.format_word.component_r),
        TIC("g_data_type", 10, 12, d.format_word.component_g),
        TIC("b_data_type", 13, 15, d.format_word.component_b),
        TIC("a_data_type", 16, 18, d.format_word.component_a),
        TIC("x_source", 19, 21, d.format_word.swizzle_x),
        TIC("y_source", 22, 24, d.format_word.swizzle_y),
        TIC("z_source", 25, 27, d.format_word.swizzle_z),
        TIC("w_source", 28, 30, d.format_word.swizzle_w),
        TIC("pack_components", 31, 31, d.format_word.pack),
        TIC("address_lo", 32, 63, d.addr_lo),
        TIC("address_hi", 64, 79, d.addr_hi),
        TIC("view_layer_base_3_7", 80, 84, d.view_layer_base_3_7),
        TIC("header_version", 85, 87, d.hdr_version),
        TIC("load_store_hint", 88, 88, d.load_store_hint_maybe),
        TIC("resource_view_coherency_hash", 89, 92, d.view_coherency_hash),
        TIC("view_layer_base_8_10", 93, 95, d.view_layer_base_8_10),
        TIC("gobs_per_block_width", 96, 98, d.tile_width_gobs_log2),
        TIC("gobs_per_block_height", 99, 101, d.tile_height_gobs_log2),
        TIC("gobs_per_block_depth", 102, 104, d.tile_depth_gobs_log2),
        TIC("tile_width_in_gobs", 106, 108, d.sparse_tile_width_gobs_log2),
        TIC("gob3d", 109, 109, d.gob_3d),
        TIC("lod_aniso_quality2", 112, 112, d.lod_aniso_quality_2),
        TIC("lod_aniso_quality", 113, 113, d.lod_aniso_quality),
        TIC("lod_iso_quality", 114, 114, d.lod_iso_quality),
        TIC("aniso_coarse_spread_modifier", 115, 116, d.aniso_coarse_spread_modifier),
        TIC("aniso_spread_scale", 117, 121, d.aniso_spread_scale),
        TIC("use_header_opt_control", 122, 122, d.use_header_opt_control),
        TIC("depth_texture", 123, 123, d.depth_texture),
        TIC("max_mip_level", 124, 127, d.mip_max_levels),
        TIC("width_minus_one", 128, 143, d.width_minus_one),
        TIC("view_layer_base_0_2", 144, 146, d.view_layer_base_0_2),
        TIC("aniso_spread_max_log2", 147, 149, d.aniso_spread_max_log2),
        TIC("srgb_conversion", 150, 150, d.is_srgb),
        TIC("texture_type", 151, 154, d.texture_type),
        TIC("sector_promotion", 155, 156, d.sector_promotion),
        TIC("border_size", 157, 159, d.border_size),
        TIC("height_minus_one", 160, 175, d.height_minus_one),
        TIC("depth_minus_one", 176, 189, d.depth_minus_one),
        TIC("is_sparse", 190, 190, d.is_sparse),
        TIC("normalized_coords", 191, 191, d.normalized_coords),
        TIC("color_key_op", 192, 192, d.color_key_op),
        TIC("trilin_opt", 193, 197, d.trilin_opt),
        TIC("mip_lod_bias", 198, 210, d.mip_lod_bias),
        TIC("aniso_bias", 211, 214, d.aniso_bias),
        TIC("aniso_fine_spread_func", 215, 216, d.aniso_fine_spread_func),
        TIC("aniso_coarse_spread_func", 217, 218, d.aniso_coarse_spread_func),
        TIC("max_anisotropy", 219, 221, d.max_anisotropy),
        TIC("aniso_fine_spread_modifier", 222, 223, d.aniso_fine_spread_modifier),
        TIC("res_view_min_mip_level", 224, 227, d.view_mip_min_level),
        TIC("res_view_max_mip_level", 228, 231, d.view_mip_max_level),
        TIC("multi_sample_count", 232, 235, d.msaa_mode),
        TIC("min_lod_clamp", 236, 247, d.min_lod_clamp),
    });
#undef TIC
#define TSC(name, lo, hi, expr) \
    DescriptorField<TextureSamplerControl, 8>{name, lo, hi, \
        [](const TextureSamplerControl& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<TextureSamplerControl, 8>("TSC (TEXSAMP0-7)", {
        TSC("address_u", 0, 2, d.address_u),
        TSC("address_v", 3, 5, d.address_v),
        TSC("address_p", 6, 8, d.address_p),
        TSC("depth_compare", 9, 9, d.depth_compare),
        TSC("depth_compare_func", 10, 12, d.depth_compare_op),
        TSC("srgb_conversion", 13, 13, d.srgb_conversion),
        TSC("max_anisotropy", 20, 22, d.max_anisotropy),
        TSC("mag_filter", 32, 34, d.mag_filter),
        TSC("min_lod_clamp_behavior", 35, 35, d.min_lod_clamp_integer_only),
        TSC("min_filter", 36, 37, d.min_filter),
        TSC("mip_filter", 38, 39, d.mip_filter),
        TSC("cubemap_interface_filtering_0", 40, 40, d.cubemap_anisotropy),
        TSC("cubemap_interface_filtering_1", 41, 41, d.cubemap_interface_filtering),
        TSC("reduction_filter", 42, 43, d.reduction_filter),
        TSC("mip_lod_bias", 44, 56, d.mip_lod_bias),
        TSC("float_coord_normalization", 57, 57, d.float_coord_normalization),
        TSC("trilin_opt", 58, 62, d.trilin_opt),
        TSC("min_lod_clamp", 64, 75, d.min_lod_clamp),
        TSC("max_lod_clamp", 76, 87, d.max_lod_clamp),
        TSC("srgb_border_color_r", 88, 95, d.srgb_border_color_r),
        TSC("srgb_border_color_g", 108, 115, d.srgb_border_color_g),
        TSC("srgb_border_color_b", 116, 123, d.srgb_border_color_b),
        TSC("border_color_r", 128, 159, d.border_color_r),
        TSC("border_color_g", 160, 191, d.border_color_g),
        TSC("border_color_b", 192, 223, d.border_color_b),
        TSC("border_color_a", 224, 255, d.border_color_a),
    });
#undef TSC
    std::cout << "PASS: TIC/TSC field layout matches clb197tex.h (53 TIC and 26 "
                 "TSC fields, no overlaps)\n";
}

// SET_VERTEX_ATTRIBUTE_A(i) (clb197.h 0x1160): STREAM 4:0, SOURCE 6, OFFSET
// 20:7, COMPONENT_BIT_WIDTHS 26:21, NUMERICAL_TYPE 29:27, SWAP_R_AND_B 31; and
// SET_COLOR_TARGET_*(j) (0x800 + j * 64)
void testVertexAttributeAndRenderTargetLayout() {
    using namespace hw::tegra_x1::gpu::engines;
#define VA(name, lo, hi, expr) \
    DescriptorField<VertexAttribState, 1>{name, lo, hi, \
        [](const VertexAttribState& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<VertexAttribState, 1>("SET_VERTEX_ATTRIBUTE_A", {
        VA("stream", 0, 4, d.buffer_id),
        VA("source", 6, 6, d.is_fixed),
        VA("offset", 7, 20, d.offset),
        VA("component_bit_widths", 21, 26, d.size),
        VA("numerical_type", 27, 29, d.type),
        VA("swap_r_and_b", 31, 31, d.bgra),
    });
#undef VA
    // Component widths and numerical types are the header's values
    expect(static_cast<u32>(VertexAttribSize::_4x32) == 0x01 &&
               static_cast<u32>(VertexAttribSize::_3x32) == 0x02 &&
               static_cast<u32>(VertexAttribSize::_4x16) == 0x03 &&
               static_cast<u32>(VertexAttribSize::_2x32) == 0x04 &&
               static_cast<u32>(VertexAttribSize::_3x16) == 0x05 &&
               static_cast<u32>(VertexAttribSize::_4x8) == 0x0a &&
               static_cast<u32>(VertexAttribSize::_2x16) == 0x0f &&
               static_cast<u32>(VertexAttribSize::_1x32) == 0x12 &&
               static_cast<u32>(VertexAttribSize::_3x8) == 0x13 &&
               static_cast<u32>(VertexAttribSize::_2x8) == 0x18 &&
               static_cast<u32>(VertexAttribSize::_1x16) == 0x1b &&
               static_cast<u32>(VertexAttribSize::_1x8) == 0x1d &&
               static_cast<u32>(VertexAttribSize::A8B8G8R8) == 0x2f &&
               static_cast<u32>(VertexAttribSize::_10_10_10_2) == 0x30 &&
               static_cast<u32>(VertexAttribSize::_11_11_10) == 0x31 &&
               static_cast<u32>(VertexAttribSize::G8R8) == 0x32 &&
               static_cast<u32>(VertexAttribSize::X8B8G8R8) == 0x33 &&
               static_cast<u32>(VertexAttribSize::A8) == 0x34,
           "Vertex component bit width encodings match clb197.h");
    expect(static_cast<u32>(VertexAttribType::Snorm) == 1 &&
               static_cast<u32>(VertexAttribType::Unorm) == 2 &&
               static_cast<u32>(VertexAttribType::Sint) == 3 &&
               static_cast<u32>(VertexAttribType::Uint) == 4 &&
               static_cast<u32>(VertexAttribType::Uscaled) == 5 &&
               static_cast<u32>(VertexAttribType::Sscaled) == 6 &&
               static_cast<u32>(VertexAttribType::Float) == 7,
           "Vertex numerical types match clb197.h");

    // Render target register block, relative to SET_COLOR_TARGET_A(0) = 0x800
    const auto rt = [&](usize offset, usize header, const char* name) {
        if (offset != header - 0x800)
            throw std::runtime_error(
                std::string("RenderTarget offset differs from clb197.h: ") + name);
    };
    rt(offsetof(RenderTarget, addr), 0x800, "SET_COLOR_TARGET_A/B");
    rt(offsetof(RenderTarget, width_or_stride), 0x808, "SET_COLOR_TARGET_WIDTH");
    rt(offsetof(RenderTarget, height), 0x80c, "SET_COLOR_TARGET_HEIGHT");
    rt(offsetof(RenderTarget, format), 0x810, "SET_COLOR_TARGET_FORMAT");
    rt(offsetof(RenderTarget, tile_mode), 0x814, "SET_COLOR_TARGET_MEMORY");
    rt(offsetof(RenderTarget, array_mode), 0x818, "SET_COLOR_TARGET_THIRD_DIMENSION");
    rt(offsetof(RenderTarget, layer_stride), 0x81c, "SET_COLOR_TARGET_ARRAY_PITCH");
    rt(offsetof(RenderTarget, base_layer), 0x820, "SET_COLOR_TARGET_LAYER");
    rt(offsetof(RenderTarget, mark), 0x824, "SET_COLOR_TARGET_MARK");
    expect(sizeof(RenderTarget) == 64, "SET_COLOR_TARGET_*(j) stride is 64 bytes");
#define MEM(name, lo, hi, expr) \
    DescriptorField<decltype(RenderTarget::tile_mode), 1>{name, lo, hi, \
        [](const decltype(RenderTarget::tile_mode)& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<decltype(RenderTarget::tile_mode), 1>(
        "SET_COLOR_TARGET_MEMORY", {
            MEM("block_width", 0, 3, d.width_gobs_log2),
            MEM("block_height", 4, 7, d.height_gobs_log2),
            MEM("block_depth", 8, 11, d.depth_gobs_log2),
            MEM("layout", 12, 12, d.is_linear),
            MEM("third_dimension_control", 16, 16, d.is_3d),
        });
#undef MEM
    std::cout << "PASS: vertex attribute and render target layout match clb197.h\n";
}

// ComputeQmd accessors against NVIDIA's clb1c0qmd.h (QMDV01_07)
void testComputeQmdLayout() {
    using namespace hw::tegra_x1::gpu::engines;
#define QMD(name, lo, hi, expr) \
    DescriptorField<ComputeQmd, 64>{name, lo, hi, \
        [](const ComputeQmd& d) { return static_cast<u64>(expr); }}
    std::vector<DescriptorField<ComputeQmd, 64>> fields = {
        QMD("program_offset", 256, 287, d.programStart()),
        QMD("cta_raster_width", 384, 415, d.gridDimX()),
        QMD("cta_raster_height", 416, 431, d.gridDimY()),
        QMD("cta_raster_depth", 432, 447, d.gridDimZ()),
        QMD("shared_memory_size", 544, 561, d.sharedAlloc()),
        QMD("cta_thread_dimension0", 592, 607, d.blockDimX()),
        QMD("cta_thread_dimension1", 608, 623, d.blockDimY()),
        QMD("cta_thread_dimension2", 624, 639, d.blockDimZ()),
        QMD("constant_buffer_valid", 640, 647, d.constBufferEnableMask()),
        QMD("shader_local_memory_low_size", 1440, 1463, d.localPosAlloc()),
    };
#undef QMD
    for (u32 i = 0; i < COMPUTE_CONST_BUFFER_COUNT; i++) {
        fields.push_back({"constant_buffer_addr", 928 + 64 * i, 967 + 64 * i,
                          [i](const ComputeQmd& d) { return d.constBufferAddress(i); }});
        fields.push_back({"constant_buffer_size", 975 + 64 * i, 991 + 64 * i,
                          [i](const ComputeQmd& d) { return u64(d.constBufferSize(i)); }});
    }
    checkDescriptorFields<ComputeQmd, 64>("QMD (QMDV01_07)", fields);
    // Compute class registers (clb1c0.h)
    const auto reg = [](usize offset, usize header, const char* name) {
        if (offset != header)
            throw std::runtime_error(
                std::string("RegsCompute offset differs from clb1c0.h: ") + name);
    };
    reg(offsetof(RegsCompute, launch_desc_loc), 0x2b4, "SEND_PCAS_A");
    reg(offsetof(RegsCompute, launch), 0x2bc, "SEND_SIGNALING_PCAS_B");
    reg(offsetof(RegsCompute, tsc_addr_hi), 0x155c, "SET_TEX_SAMPLER_POOL_A");
    reg(offsetof(RegsCompute, tsc_limit), 0x1564, "SET_TEX_SAMPLER_POOL_C");
    reg(offsetof(RegsCompute, tic_addr_hi), 0x1574, "SET_TEX_HEADER_POOL_A");
    reg(offsetof(RegsCompute, tic_limit), 0x157c, "SET_TEX_HEADER_POOL_C");
    reg(offsetof(RegsCompute, code_addr_hi), 0x1608, "SET_PROGRAM_REGION_A");
    reg(offsetof(RegsCompute, tex_cb_index), 0x2608, "SET_BINDLESS_TEXTURE");
    std::cout << "PASS: compute QMD accessors match clb1c0qmd.h (" << fields.size()
              << " fields, no overlaps)\n";
}

// DMA copy engine registers and packed words against NVIDIA's clb0b5.h
// (MAXWELL_DMA_COPY_A)
void testCopyEngineLayout() {
    using namespace hw::tegra_x1::gpu::engines;
    const auto reg = [](usize offset, usize header, const char* name) {
        if (offset != header)
            throw std::runtime_error(
                std::string("RegsCopy offset differs from clb0b5.h: ") + name);
    };
    reg(offsetof(RegsCopy, offset_in), 0x400, "OFFSET_IN_UPPER");
    reg(offsetof(RegsCopy, offset_out), 0x408, "OFFSET_OUT_UPPER");
    reg(offsetof(RegsCopy, stride_in), 0x410, "PITCH_IN");
    reg(offsetof(RegsCopy, stride_out), 0x414, "PITCH_OUT");
    reg(offsetof(RegsCopy, line_length_in), 0x418, "LINE_LENGTH_IN");
    reg(offsetof(RegsCopy, line_count), 0x41c, "LINE_COUNT");
    reg(offsetof(RegsCopy, remap_const1), 0x700, "SET_REMAP_CONST_A");
    reg(offsetof(RegsCopy, remap_const2), 0x704, "SET_REMAP_CONST_B");
    reg(offsetof(RegsCopy, remap_components), 0x708, "SET_REMAP_COMPONENTS");
    reg(offsetof(RegsCopy, dst), 0x70c, "SET_DST_BLOCK_SIZE");
    reg(offsetof(RegsCopy, dst) + offsetof(TextureCopyInfo, stride), 0x710, "SET_DST_WIDTH");
    reg(offsetof(RegsCopy, dst) + offsetof(TextureCopyInfo, height), 0x714, "SET_DST_HEIGHT");
    reg(offsetof(RegsCopy, dst) + offsetof(TextureCopyInfo, depth), 0x718, "SET_DST_DEPTH");
    reg(offsetof(RegsCopy, dst) + offsetof(TextureCopyInfo, layer), 0x71c, "SET_DST_LAYER");
    reg(offsetof(RegsCopy, dst) + offsetof(TextureCopyInfo, origin), 0x720, "SET_DST_ORIGIN");
    reg(offsetof(RegsCopy, src), 0x728, "SET_SRC_BLOCK_SIZE");
    reg(offsetof(RegsCopy, src) + offsetof(TextureCopyInfo, stride), 0x72c, "SET_SRC_WIDTH");
    reg(offsetof(RegsCopy, src) + offsetof(TextureCopyInfo, origin), 0x73c, "SET_SRC_ORIGIN");
    using Remap = decltype(RegsCopy::remap_components);
#define REMAP(name, lo, hi, expr) \
    DescriptorField<Remap, 1>{name, lo, hi, [](const Remap& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<Remap, 1>("SET_REMAP_COMPONENTS", {
        REMAP("dst_x", 0, 2, d.dst_x),
        REMAP("dst_y", 4, 6, d.dst_y),
        REMAP("dst_z", 8, 10, d.dst_z),
        REMAP("dst_w", 12, 14, d.dst_w),
        REMAP("component_size", 16, 17, d.component_size_minus_one),
        REMAP("num_src_components", 20, 21, d.src_component_count_minus_one),
        REMAP("num_dst_components", 24, 25, d.dst_component_count_minus_one),
    });
#undef REMAP
#define BLK(name, lo, hi, expr) \
    DescriptorField<BlockSize, 1>{name, lo, hi, [](const BlockSize& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<BlockSize, 1>("SET_*_BLOCK_SIZE", {
        BLK("width", 0, 3, d.width),
        BLK("height", 4, 7, d.height),
        BLK("depth", 8, 11, d.depth),
        BLK("gob_height", 12, 15, d.gob_height),
    });
#undef BLK
    using Origin = decltype(TextureCopyInfo::origin);
#define ORG(name, lo, hi, expr) \
    DescriptorField<Origin, 1>{name, lo, hi, [](const Origin& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<Origin, 1>("SET_*_ORIGIN", {
        ORG("x", 0, 15, d.x),
        ORG("y", 16, 31, d.y),
    });
#undef ORG
    std::cout << "PASS: DMA copy engine layout matches clb0b5.h\n";
}

// 2D engine registers against NVIDIA's cl902d.h (FERMI_TWOD_A)
void testTwoDEngineLayout() {
    using namespace hw::tegra_x1::gpu::engines;
    const auto reg = [](usize offset, usize header, const char* name) {
        if (offset != header)
            throw std::runtime_error(
                std::string("Regs2D offset differs from cl902d.h: ") + name);
    };
    reg(offsetof(Regs2D, dst), 0x200, "SET_DST_FORMAT");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, depth), 0x20c, "SET_DST_DEPTH");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, layer), 0x210, "SET_DST_LAYER");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, stride), 0x214, "SET_DST_PITCH");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, width), 0x218, "SET_DST_WIDTH");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, height), 0x21c, "SET_DST_HEIGHT");
    reg(offsetof(Regs2D, dst) + offsetof(Texture2DInfo, addr), 0x220, "SET_DST_OFFSET_UPPER");
    reg(offsetof(Regs2D, src), 0x230, "SET_SRC_FORMAT");
    reg(offsetof(Regs2D, src) + offsetof(Texture2DInfo, depth), 0x23c, "SET_SRC_DEPTH");
    reg(offsetof(Regs2D, src) + offsetof(Texture2DInfo, stride), 0x244, "SET_SRC_PITCH");
    reg(offsetof(Regs2D, src) + offsetof(Texture2DInfo, width), 0x248, "SET_SRC_WIDTH");
    reg(offsetof(Regs2D, src) + offsetof(Texture2DInfo, height), 0x24c, "SET_SRC_HEIGHT");
    reg(offsetof(Regs2D, clip_x), 0x280, "SET_CLIP_X");
    reg(offsetof(Regs2D, clip_width), 0x288, "SET_CLIP_WIDTH");
    reg(offsetof(Regs2D, clip_enabled), 0x290, "SET_CLIP_ENABLE");
    reg(offsetof(Regs2D, color_key_format), 0x294, "SET_COLOR_KEY_FORMAT");
    reg(offsetof(Regs2D, color_key), 0x298, "SET_COLOR_KEY");
    reg(offsetof(Regs2D, color_key_enabled), 0x29c, "SET_COLOR_KEY_ENABLE");
    reg(offsetof(Regs2D, rop), 0x2a0, "SET_ROP");
    reg(offsetof(Regs2D, beta1), 0x2a4, "SET_BETA1");
    reg(offsetof(Regs2D, beta4), 0x2a8, "SET_BETA4");
    reg(offsetof(Regs2D, operation), 0x2ac, "SET_OPERATION");
    reg(offsetof(Regs2D, sample_mode), 0x88c, "SET_PIXELS_FROM_MEMORY_SAMPLE_MODE");
    reg(offsetof(Regs2D, pixels_from_memory), 0x8b0, "SET_PIXELS_FROM_MEMORY_DST_X0");
    using PFM = PixelsFromMemory;
    reg(offsetof(PFM, dst_width), 0x8, "..._DST_WIDTH (+0x8b0)");
    reg(offsetof(PFM, dudx), 0x10, "..._DU_DX_FRAC (+0x8b0)");
    reg(offsetof(PFM, dvdy), 0x18, "..._DV_DY_FRAC (+0x8b0)");
    reg(offsetof(PFM, src_x0), 0x20, "..._SRC_X0_FRAC (+0x8b0)");
    reg(offsetof(PFM, src_y0), 0x28, "..._SRC_Y0_FRAC (+0x8b0)");
    reg(offsetof(hw::tegra_x1::gpu::engines::FixedPoint, integer), 0x4, "FixedPoint integer word follows the fraction");
    using Sample = decltype(Regs2D::sample_mode);
#define SMP(name, lo, hi, expr) \
    DescriptorField<Sample, 1>{name, lo, hi, [](const Sample& d) { return static_cast<u64>(expr); }}
    checkDescriptorFields<Sample, 1>("SET_PIXELS_FROM_MEMORY_SAMPLE_MODE", {
        SMP("origin", 0, 0, d.origin),
        SMP("filter", 4, 4, d.filter),
    });
#undef SMP
    std::cout << "PASS: 2D engine layout matches cl902d.h\n";
}

// Inline-to-memory registers shared by the 3D, compute and A140 classes
// (cla140.h, KEPLER_INLINE_TO_MEMORY_B)
void testInlineEngineLayout() {
    using namespace hw::tegra_x1::gpu::engines;
    const auto reg = [](usize offset, usize header, const char* name) {
        if (offset != header)
            throw std::runtime_error(
                std::string("RegsInline offset differs from cla140.h: ") + name);
    };
    reg(offsetof(RegsInline, line_length_in), 0x180, "LINE_LENGTH_IN");
    reg(offsetof(RegsInline, line_count), 0x184, "LINE_COUNT");
    reg(offsetof(RegsInline, offset_out), 0x188, "OFFSET_OUT_UPPER");
    reg(offsetof(RegsInline, pitch_out), 0x190, "PITCH_OUT");
    reg(offsetof(RegsInline, dst_block_size), 0x194, "SET_DST_BLOCK_SIZE");
    reg(offsetof(RegsInline, dst_width), 0x198, "SET_DST_WIDTH");
    reg(offsetof(RegsInline, dst_height), 0x19c, "SET_DST_HEIGHT");
    reg(offsetof(RegsInline, dst_depth), 0x1a0, "SET_DST_DEPTH");
    reg(offsetof(RegsInline, dst_layer), 0x1a4, "SET_DST_LAYER");
    reg(offsetof(RegsInline, dst_origin_bytes_x), 0x1a8, "SET_DST_ORIGIN_BYTES_X");
    reg(offsetof(RegsInline, dst_origin_samples_y), 0x1ac, "SET_DST_ORIGIN_SAMPLES_Y");
    reg(offsetof(RegsInline, not_a_reg_0xc), 0x1b0, "LAUNCH_DMA");
    reg(offsetof(RegsInline, not_a_reg_0xd), 0x1b4, "LOAD_INLINE_DATA");
    std::cout << "PASS: inline-to-memory layout matches cla140.h\n";
}

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
        testConstBufferRemapping();
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
        testExplicitAlphaBlendEquation();
        testRegisterLayout();
        testTextureDescriptorLayout();
        testVertexAttributeAndRenderTargetLayout();
        testComputeQmdLayout();
        testCopyEngineLayout();
        testTwoDEngineLayout();
        testInlineEngineLayout();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

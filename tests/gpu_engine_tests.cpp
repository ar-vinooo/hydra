// Engine-level GPU tests: the macro (MME) interpreter and renderer operations
// the engines issue (clears), checked on Metal where pixels are involved.
#include "core/hw/tegra_x1/gpu/capture/draw_capture.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/clear.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/compute.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/copy.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/draw.hpp"
#include "core/hw/tegra_x1/gpu/engines/2d.hpp"
#include "core/hw/tegra_x1/gpu/engines/compute.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/macro/interpreter/driver.hpp"
#include "core/hw/tegra_x1/gpu/management/metal_submission.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/optimization_policy.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/draw_state.hpp"
#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/completion_queue.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace hydra::hw::tegra_x1::gpu::engines {
struct ThreeDTestAccess {
    static void execute(ThreeD& engine, command_ir::DrawCommand& command) {
        engine.executeDrawCommand(command);
    }
    static void colorTarget(ThreeD& engine, u64 address) {
        engine.regs.color_target_control.count = 1;
        engine.regs.color_target_control.maps = 0;
        auto& target = engine.regs.color_targets[0];
        target = {};
        target.addr = {static_cast<u32>(address >> 32), static_cast<u32>(address)};
        target.width_or_stride = 16;
        target.height = 4;
        target.format = ColorSurfaceFormat::RGBA8Unorm;
        target.tile_mode.is_linear = 1;
        target.array_mode.layers = 1;
        engine.regs.depth_target_enabled = 0;
    }
    static renderer::RenderPassBase* targets(ThreeD& engine) {
        auto* pass = engine.resolveDrawTargets();
        engine.texture_memo_epoch = engine.gpu.getRenderer().getTextureCache().getEpoch();
        engine.texture_memo_render_pass = pass;
        engine.texture_memo_gmmu = tls_crnt_gmmu;
        engine.texture_memo_mapping_generation = tls_crnt_gmmu->getMappingGeneration();
        engine.texture_memo_address_space = engine.gpu.getRenderer().getResourceRegistry().track(
            tls_crnt_gmmu->getResourceLifetime(), resource::ResourceKind::AddressSpace, tls_crnt_gmmu);
        engine.render_target_memo_key.swap(engine.render_target_key);
        return pass;
    }
    static command_ir::StageBindings fragmentBindings(ThreeD& engine,
                                                     renderer::ShaderBase* shader) {
        const auto index = static_cast<u32>(renderer::ShaderType::Fragment);
        auto* previous = engine.active_shaders[index];
        engine.active_shaders[index] = shader;
        command_ir::StageBindings bindings;
        std::vector<ztd::Range<uptr>> writes;
        engine.configureShaderStage(ShaderStage::Fragment, nullptr, nullptr,
                                    bindings, writes);
        engine.active_shaders[index] = previous;
        return bindings;
    }
};
} // namespace hydra::hw::tegra_x1::gpu::engines

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
    renderer::ShaderDescriptor descriptor{};
    descriptor.type = renderer::ShaderType::Fragment;
    descriptor.backend = ShaderBackend::Msl;
    const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 main_(constant uint4* values [[buffer(1)]]) {
    return float4(values[0]);
})";
    descriptor.code.assign(source.begin(), source.end());
    descriptor.resource_mapping.uniform_buffers[SLOT] = 1;
    descriptor.resource_mapping.uniform_buffer_sizes[SLOT] = SIZE;
    std::unique_ptr<renderer::ShaderBase> shader(renderer.createShader(descriptor));
    const auto recorded_before = engines::ThreeDTestAccess::fragmentBindings(engine, shader.get());
    expect(recorded_before.active && recorded_before.reset_textures &&
               recorded_before.uniforms.size() == 1 && recorded_before.uniforms[0].index == 1 &&
               recorded_before.storage.empty() && recorded_before.textures.empty(),
           "Engine records declared host bindings and an explicit empty texture snapshot");
    const auto before = engine.getConstBufferUpload(STAGE, SLOT);
    expect(*reinterpret_cast<const u32*>(before.getPtr()) == original[4],
           "Initial constant buffer upload reads original backing");

    gmmu.map(ADDRESS, {new_ptr, sizeof(replacement)});
    check_range(new_ptr + OFFSET);
    const auto after = engine.getConstBufferUpload(STAGE, SLOT);
    const auto recorded_after = engines::ThreeDTestAccess::fragmentBindings(engine, shader.get());
    expect(*reinterpret_cast<const u32*>(recorded_before.uniforms[0].buffer.getPtr()) == original[4] &&
               *reinterpret_cast<const u32*>(recorded_after.uniforms[0].buffer.getPtr()) == replacement[4],
           "Recorded engine bindings keep old snapshots while later commands resolve remapped memory");
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
    metal_commands->debugCommitAndWait();
    const bool completed = metal_commands->lastSubmissionSucceeded();
    tls_crnt_command_buffer = nullptr;
    commands.reset();
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
    expect(engine.getConstBuffer(STAGE, SLOT, 16).getBegin() == new_ptr + OFFSET &&
               engine.getConstBuffer(STAGE, SLOT, 16).getSize() == 16 &&
               engine.getConstBuffer(STAGE, SLOT, 17).getSize() == 0,
           "Direct shader reads validate their prefix without requiring an unmapped tail");
    bind(ADDRESS + OFFSET, 0x10000);
    const auto prefix_upload = engine.getConstBufferUpload(STAGE, SLOT, 16);
    expect(prefix_upload.isValid() && prefix_upload.getSize() == 16 &&
               std::memcmp(reinterpret_cast<const void*>(prefix_upload.getPtr()),
                           replacement.data() + 4, 16) == 0,
           "A 64 KiB selector uploads mapped shader constants from a smaller allocation");
    auto* prefix_commands = static_cast<renderer::metal::CommandBuffer*>(commands.get());
    auto* prefix_encoder = prefix_commands->getComputeCommandEncoder();
    prefix_encoder->setComputePipelineState(pipeline);
    prefix_encoder->setBuffer(
        static_cast<renderer::metal::Buffer*>(prefix_upload.getBase())->getBuffer(),
        prefix_upload.getOffset(), 0);
    prefix_encoder->setBuffer(output, 0, 1);
    prefix_encoder->dispatchThreads(MTL::Size(4, 1, 1), MTL::Size(4, 1, 1));
    prefix_commands->debugCommitAndWait();
    const bool prefix_completed = prefix_commands->lastSubmissionSucceeded();
    tls_crnt_command_buffer = nullptr;
    commands.reset();
    expect(prefix_completed &&
               std::memcmp(output->contents(), replacement.data() + 4, 16) == 0,
           "Metal reads the complete required prefix despite an unmapped selector tail");
    commands.reset(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    expect(engine.getConstBuffer(STAGE, SLOT, 0x10001).getSize() == 0 &&
               engine.getConstBuffer(STAGE, SLOT).getSize() == 0,
           "Reads beyond the declared size and unbounded reads still reject missing memory");
    bind(ADDRESS + OFFSET, SIZE);
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

// A 4x4 render target with depth, a full screen triangle at depth 0.5 and the
// report memory, for the occlusion tests. The triangle counts 16 samples when
// the depth buffer holds 0.75, none when it holds 0.25, and 4 with a 2x2
// scissor.
struct OcclusionRig {
    static constexpr u64 REPORT = 0x400000;
    // The test fixture installs the thread locals the engines use
    explicit OcclusionRig(usize report_bytes)
        : renderer{static_cast<hydra::hw::tegra_x1::gpu::renderer::metal::Renderer&>(
              gpu.getRenderer())},
          engine{gpu.get3DEngine()}, image_memory(0x20000, 0),
          report_memory(report_bytes, 0), gmmu(nullptr) {
        using namespace hydra::hw::tegra_x1::gpu::renderer;
        gmmu.map(REPORT, {reinterpret_cast<uptr>(report_memory.data()),
                          report_memory.size()});
        commands.reset(renderer.createCommandBuffer());
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
        pass.reset(renderer.createRenderPass(pass_descriptor));
        renderer.bindRenderPass(pass.get());
        const auto shader = [&](ShaderType type, const std::string& code) {
            ShaderDescriptor descriptor{};
            descriptor.type = type;
            descriptor.backend = ShaderBackend::Msl;
            descriptor.code.assign(code.begin(), code.end());
            return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
        };
        vertex = shader(ShaderType::Vertex, R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]]) {
    float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[id], 0.5, 1);
})");
        fragment = shader(ShaderType::Fragment, R"(
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
        pipeline.reset(renderer.createPipeline(pipeline_descriptor));
        renderer.bindPipeline(pipeline.get());
        setScissor(full);
        for (u32 i = 0; i < VIEWPORT_COUNT; i++)
            renderer.setViewport(i, MTL::Viewport{0, 0, 4, 4, 0, 1});
        renderer.setCullState(false, engines::CullFaceMode::Back, engines::Winding::CounterClockwise);
        renderer.setDepthTestEnabled(true);
        renderer.setDepthWriteEnabled(false);
        renderer.setDepthCompareOp(engines::CompareOp::Less);
        renderer.setStencilState({});
        renderer.setClearState({.rect = full});
        renderer.clearDepth(commands.get(), 0, 0.25f);
        tls_crnt_gmmu = &gmmu;
    }
    ~OcclusionRig() {
        // Reports still on their way write to the memory and address space of
        // the rig, also when a test failed
        commands.reset();
        renderer.drainReports();
        tls_crnt_command_buffer = nullptr;
        tls_crnt_gmmu = nullptr;
    }
    OcclusionRig(const OcclusionRig&) = delete;
    OcclusionRig& operator=(const OcclusionRig&) = delete;

    void setScissor(const MTL::ScissorRect& rect) {
        for (u32 i = 0; i < VIEWPORT_COUNT; i++)
            renderer.setScissor(i, rect);
    }

    const MTL::ScissorRect full{0, 0, 4, 4};
    Gpu gpu;
    hydra::hw::tegra_x1::gpu::renderer::metal::Renderer& renderer;
    hydra::hw::tegra_x1::gpu::engines::ThreeD& engine;
    std::vector<u8> image_memory;
    std::vector<u8> report_memory;
    GMmu gmmu;
    std::unique_ptr<hydra::hw::tegra_x1::gpu::renderer::ICommandBuffer> commands;
    std::unique_ptr<hydra::hw::tegra_x1::gpu::renderer::RenderPassBase> pass;
    std::unique_ptr<hydra::hw::tegra_x1::gpu::renderer::ShaderBase> vertex;
    std::unique_ptr<hydra::hw::tegra_x1::gpu::renderer::ShaderBase> fragment;
    std::unique_ptr<hydra::hw::tegra_x1::gpu::renderer::PipelineBase> pipeline;
};

void testDrawStateSnapshots() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    std::vector<u8> baseline;
    for (const bool snapshot : {false, true}) {
        OcclusionRig rig(32);
        auto& renderer = rig.renderer;
        auto* commands = static_cast<metal::CommandBuffer*>(rig.commands.get());
        renderer.setClearState({.rect = rig.full});
        renderer.clearColor(commands, 0, 0, 0xf, simd::uint4{});
        DrawState state{};
        state.depth_test_enabled = true;
        state.depth_compare_op = engines::CompareOp::Less;
        state.viewports.fill({0, 0, 4, 4, 0, 1});
        state.scissors.fill(rig.full);
        const auto apply = [&](const DrawState& value) {
            if (snapshot) {
                applyDrawState(renderer, value);
            } else {
                // The setters used by drawInternal before the snapshot split.
                renderer.setDepthTestEnabled(value.depth_test_enabled);
                renderer.setDepthWriteEnabled(value.depth_write_enabled);
                renderer.setDepthCompareOp(value.depth_compare_op);
                renderer.setBlendConstant(value.blend_constant);
                renderer.setStencilState(value.stencil);
                renderer.setCullState(value.cull_enabled, value.cull_mode,
                                      value.front_face);
                renderer.setDepthBias(value.depth_bias_constant,
                                      value.depth_bias_slope, value.depth_bias_clamp);
                renderer.setDepthClamp(value.depth_clamp);
                for (u32 i = 0; i < VIEWPORT_COUNT; i++) {
                    renderer.setViewport(i, value.viewports[i]);
                    renderer.setScissor(i, value.scissors[i]);
                }
            }
        };
        const auto draw = [&] {
            renderer.draw(commands, engines::PrimitiveType::Triangles, 0, 3, 0, 1);
        };
        apply(state);
        draw(); // Rejected by the depth buffer (0.5 >= 0.25).
        state.depth_test_enabled = false;
        state.scissors.fill({0, 0, 2, 2});
        const auto saved = state;
        apply(saved);
        draw();
        state.depth_test_enabled = true;
        state.scissors.fill(rig.full);
        apply(state);
        commands->getBlitCommandEncoder();
        apply(saved); // Restore by value after mutation and encoder replacement.
        draw();
        state = saved;
        state.viewports.fill({2, 2, 2, 2, 0, 1});
        state.scissors.fill(rig.full);
        apply(state);
        draw();
        expect(renderer.synchronize(commands), "Complete snapshot raster draws");
        auto* color = static_cast<metal::TextureView*>(
            rig.pass->getDescriptor().color_targets[0].texture)->getTexture();
        const auto pixels = readTexture(renderer.getDevice(), color, 4);
        for (usize y = 0; y < 4; y++) {
            for (usize x = 0; x < 4; x++) {
                const u8 expected = ((x < 2 && y < 2) || (x >= 2 && y >= 2))
                    ? 255 : 0;
                for (usize component = 0; component < 4; component++)
                    expect(pixels[(y * 4 + x) * 4 + component] == expected,
                           "Snapshot depth, scissor and viewport produce expected pixels");
            }
        }
        if (!snapshot)
            baseline = pixels;
        else
            expect(pixels == baseline, "Snapshot and legacy raster paths match byte for byte");
    }
    std::cout << "PASS: draw-state snapshots match legacy pixels after mutation and encoder replacement\n";
}

void testDrawCommands() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using command_ir::DrawCommand;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    // Empty commands do not touch the renderer or require resources.
    {
        metal::Renderer renderer;
        command_ir::executeDraw(renderer, nullptr, {});
    }
    std::vector<u8> baseline;
    for (const auto index_type : {engines::IndexType::None,
                                 engines::IndexType::UInt16,
                                 engines::IndexType::UInt32}) {
        for (const bool recorded_path : {false, true}) {
            OcclusionRig rig(32);
            auto& renderer = rig.renderer;
            auto* commands = static_cast<metal::CommandBuffer*>(rig.commands.get());
            auto make_shader = [&](ShaderType type, const char* source) {
                ShaderDescriptor descriptor{};
                descriptor.type = type;
                descriptor.backend = ShaderBackend::Msl;
                descriptor.code.assign(source, source + std::strlen(source));
                if (type == ShaderType::Fragment) {
                    descriptor.resource_mapping.uniform_buffers[1] = 1;
                    descriptor.resource_mapping.storage_buffers.push_back({0, 0, 2, true});
                    descriptor.resource_mapping.textures.emplace(0, 0);
                }
                return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
            };
            auto vertex = make_shader(ShaderType::Vertex, R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; uint instance [[flat]]; };
vertex Out main_(uint id [[vertex_id]], uint instance [[instance_id]],
                 device const float4* positions [[buffer(30)]]) {
    float4 p = positions[id];
    p.x = p.x * 0.5f + (instance == 7 ? -0.5f : 0.5f);
    return {p, instance};
}
)");
            auto fragment = make_shader(ShaderType::Fragment, R"(
#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; uint instance [[flat]]; };
fragment float4 main_(In in [[stage_in]], constant float4* palette [[buffer(1)]],
                     device atomic_uint* samples [[buffer(2)]],
                     texture2d<float> image [[texture(0)]], sampler s [[sampler(0)]]) {
    atomic_fetch_add_explicit(samples, 1u, memory_order_relaxed);
    return palette[in.instance == 7 ? 0 : 1] * image.sample(s, float2(0.5f));
}
)");
            auto pipeline_descriptor = rig.pipeline->getDescriptor();
            pipeline_descriptor.shaders[0] = vertex.get();
            pipeline_descriptor.shaders[1] = fragment.get();
            std::unique_ptr<PipelineBase> pipeline(renderer.createPipeline(pipeline_descriptor));
            const auto buffer = [&](const void* data, u64 size) {
                auto* native = renderer.getDevice()->newBuffer(
                    data, size, MTL::ResourceStorageModeShared);
                expect(native != nullptr, "Create shared command test buffer");
                return std::shared_ptr<BufferBase>(new metal::Buffer(native));
            };
            // One float4 of padding is outside the view. Vertex IDs 3..6
            // select the rectangle; the earlier entries deliberately collapse.
            const std::array<simd::float4, 8> positions{{{}, {}, {}, {},
                {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1},
                {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}}};
            const std::array<simd::float4, 3> colors{{{}, {1, 0, 0, 1}, {0, 1, 0, 1}}};
            const std::array<u32, 4> zeros{};
            auto vertices = buffer(positions.data(), sizeof(positions));
            auto palette = buffer(colors.data(), sizeof(colors));
            auto samples = buffer(zeros.data(), sizeof(zeros));
            const std::weak_ptr<BufferBase> vertex_lifetime = vertices;
            const std::weak_ptr<BufferBase> palette_lifetime = palette;
            const std::weak_ptr<BufferBase> storage_lifetime = samples;
            const auto samples_ptr = samples->getPtr();
            std::array<u8, 4> white{255, 255, 255, 255};
            auto* image = renderer.getTextureCache().find(commands,
                TextureDescriptor(reinterpret_cast<uptr>(white.data()),
                    MTL::TextureType2D, TextureFormat::RGBA8Unorm, true, 4,
                    1, 1, 1, 1, 1, 0, 0, 0), TextureUsage::Read);
            SamplerDescriptor sampler_descriptor{};
            sampler_descriptor.depth_compare_op = engines::CompareOp::Always;
            std::unique_ptr<SamplerBase> sampler(renderer.createSampler(sampler_descriptor));
            DrawCommand command;
            command.pipeline = pipeline.get();
            command.render_pass = rig.pass.get();
            command.state.viewports.fill({0, 0, 4, 4, 0, 1});
            command.state.scissors.fill(rig.full);
            command.vertices[0] = BufferView(vertices, sizeof(simd::float4));
            command.stages[0].active = true;
            command.stages[1].active = true;
            command.stages[1].reset_textures = true;
            command.stages[1].uniforms.push_back({1, BufferView(palette, sizeof(simd::float4))});
            command.stages[1].storage.push_back({2, BufferView(samples)});
            command.stages[1].textures.push_back({0, image, sampler.get()});
            command.storage_writes.push_back(ztd::Range<uptr>::fromSize(samples_ptr, sizeof(u32)));
            command.count = 4;
            command.primitive = engines::PrimitiveType::TriangleStrip;
            command.first = 3;
            command.base_instance = 7;
            command.instance_count = 2;
            command.index_type = index_type;
            if (index_type != engines::IndexType::None) {
                std::shared_ptr<BufferBase> indices;
                if (index_type == engines::IndexType::UInt16) {
                    const std::array<u16, 7> values{99, 99, 88, 0, 1, 2, 3};
                    indices = buffer(values.data(), sizeof(values));
                } else {
                    const std::array<u32, 7> values{99, 99, 88, 0, 1, 2, 3};
                    indices = buffer(values.data(), sizeof(values));
                }
                command.indices = BufferView(indices, 2 * engines::getIndexTypeSize(index_type));
                command.first = 1;
                command.base_vertex = 3;
            }
            expect(command_ir::sealDrawResources(renderer, command),
                   "Seal live draw resources before recording");
            const auto recorded = command;
            command = {};
            vertices.reset();
            palette.reset();
            samples.reset();
            expect(!vertex_lifetime.expired() && !palette_lifetime.expired() &&
                       !storage_lifetime.expired(),
                   "Recorded commands retain owned buffer views after caller release");
            renderer.setClearState({.rect = rig.full});
            renderer.clearColor(commands, 0, 0, 0xf, simd::uint4{});
            const auto checkpoints = metal::CommandBuffer::getCheckpointCount();
            const auto epoch = renderer.getTextureCache().getEpoch();
            if (recorded_path) {
                expect(command_ir::executeDraw(renderer, commands, recorded),
                       "Encode a validated recorded command");
            } else {
                renderer.bindRenderPass(recorded.render_pass);
                renderer.bindPipeline(recorded.pipeline);
                applyDrawState(renderer, recorded.state);
                renderer.bindVertexBuffer(recorded.vertices[0], 0);
                renderer.unbindUniformBuffers(ShaderType::Fragment);
                renderer.bindUniformBuffer(recorded.stages[1].uniforms[0].buffer, ShaderType::Fragment, 1);
                renderer.unbindStorageBuffers(ShaderType::Fragment);
                renderer.bindStorageBuffer(recorded.stages[1].storage[0].buffer, ShaderType::Fragment, 2);
                renderer.unbindTextures(ShaderType::Fragment);
                renderer.bindTexture(image, sampler.get(), ShaderType::Fragment, 0);
                if (index_type == engines::IndexType::None) {
                    renderer.draw(commands, recorded.primitive, 3, 4, 7, 2);
                } else {
                    renderer.bindIndexBuffer(recorded.indices, index_type);
                    renderer.drawIndexed(commands, recorded.primitive, 1, 4, 3, 7, 2);
                }
                renderer.getTextureCache().synchronizeStorageWrites(commands, recorded.storage_writes);
            }
            expect(metal::CommandBuffer::getCheckpointCount() > checkpoints &&
                       renderer.getTextureCache().getEpoch() > epoch,
                   "Draw command completes storage writes and invalidates aliases");
            u32 count = 0;
            std::memcpy(&count, reinterpret_cast<const void*>(samples_ptr), sizeof(count));
            expect(count == 16, "Draw command runs exactly two instances and publishes storage writes");
            auto* color = static_cast<metal::TextureView*>(
                rig.pass->getDescriptor().color_targets[0].texture)->getTexture();
            const auto pixels = readTexture(renderer.getDevice(), color, 4);
            for (usize y = 0; y < 4; y++) {
                for (usize x = 0; x < 4; x++) {
                    const usize offset = (y * 4 + x) * 4;
                    expect(pixels[offset] == (x < 2 ? 255 : 0) &&
                               pixels[offset + 1] == (x < 2 ? 0 : 255) &&
                               pixels[offset + 2] == 0 && pixels[offset + 3] == 255,
                           "Draw command restores vertex/uniform/storage/texture bindings and draw parameters");
                }
            }
            if (!recorded_path)
                baseline = pixels;
            else
                expect(pixels == baseline, "Direct and indexed command output matches legacy byte for byte");
        }
    }
    std::cout << "PASS: direct/U16/U32 draw commands preserve bindings, offsets, instances, ownership and storage completion\n";
}

void testRenderTargetMappingMemo() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    std::array<u8, 64> old_data{}, new_data{};
    constexpr u64 address = 0x500000;
    rig.gmmu.map(address, {reinterpret_cast<uptr>(old_data.data()), old_data.size()});
    engines::ThreeDTestAccess::colorTarget(rig.engine, address);
    std::vector<resource::MappingDependency> first, cached, remapped;
    RenderPassBase* first_pass;
    {
        resource::MappingCaptureScope capture(&rig.gmmu, first);
        first_pass = engines::ThreeDTestAccess::targets(rig.engine);
    }
    expect(!first.empty(), "Render target lookup records its backing mapping");
    {
        resource::MappingCaptureScope capture(&rig.gmmu, cached);
        expect(engines::ThreeDTestAccess::targets(rig.engine) == first_pass,
               "Unchanged render targets reuse the pass");
    }
    expect(!cached.empty() && cached[0].ptr == reinterpret_cast<uptr>(old_data.data()),
           "Memo hits carry mapping dependencies into the next command");
    rig.gmmu.map(address, {reinterpret_cast<uptr>(new_data.data()), new_data.size()});
    {
        resource::MappingCaptureScope capture(&rig.gmmu, remapped);
        auto* next = engines::ThreeDTestAccess::targets(rig.engine);
        expect(next != first_pass &&
                   next->getDescriptor().color_targets[0].texture->getBase()->getDescriptor().ptr ==
                       reinterpret_cast<uptr>(new_data.data()),
               "A new GMMU generation resolves render targets against new backing");
    }
    expect(!resource::validateMappings(rig.gmmu, cached) &&
               resource::validateMappings(rig.gmmu, remapped),
           "Cached and newly resolved target mappings keep separate versions");
    {
        alignas(GMmu) std::array<std::byte, sizeof(GMmu)> storage;
        const auto destroy = [](GMmu* mmu) { std::destroy_at(mmu); };
        std::unique_ptr<GMmu, decltype(destroy)> reused(
            std::construct_at(reinterpret_cast<GMmu*>(storage.data()), nullptr), destroy);
        struct RestoreContext {
            GMmu* previous;
            ~RestoreContext() { tls_crnt_gmmu = previous; }
        } restore{tls_crnt_gmmu};
        tls_crnt_gmmu = reused.get();
        reused->map(address, {reinterpret_cast<uptr>(old_data.data()), old_data.size()});
        const auto generation = reused->getMappingGeneration();
        auto* before = engines::ThreeDTestAccess::targets(rig.engine);
        reused.reset();
        reused.reset(std::construct_at(reinterpret_cast<GMmu*>(storage.data()), nullptr));
        reused->map(address, {reinterpret_cast<uptr>(new_data.data()), new_data.size()});
        expect(reused->getMappingGeneration() == generation,
               "Reused GMMU address starts with the same numeric mapping generation");
        auto* after = engines::ThreeDTestAccess::targets(rig.engine);
        expect(after != before &&
                   after->getDescriptor().color_targets[0].texture->getBase()->getDescriptor().ptr ==
                       reinterpret_cast<uptr>(new_data.data()),
               "Registry identity prevents memo revival when a GMMU address is reused");
    }
    std::cout << "PASS: render target memo preserves dependencies and follows GMMU remaps\n";
}

void testDrawResourceLeases() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    command_ir::DrawCommand command;
    std::vector<std::weak_ptr<void>> lifetimes;
    const auto expired = [&] {
        return std::all_of(lifetimes.begin(), lifetimes.end(),
                           [](const auto& owner) { return owner.expired(); });
    };
    {
        PipelineCache pipelines(rig.renderer);
        RenderPassCache passes(rig.renderer);
        SamplerCache samplers(rig.renderer);
        TextureCache textures(rig.renderer);
        auto* vertex = rig.renderer.createShader(rig.vertex->getDescriptor());
        auto vertex_owner = vertex->createResourceOwner();
        auto* fragment = rig.renderer.createShader(rig.fragment->getDescriptor());
        auto fragment_owner = fragment->createResourceOwner();
        auto pipeline_descriptor = rig.pipeline->getDescriptor();
        pipeline_descriptor.shaders[0] = vertex;
        pipeline_descriptor.shaders[1] = fragment;
        command.pipeline = pipelines.find(pipeline_descriptor);
        auto pass_descriptor = rig.pass->getDescriptor();
        pass_descriptor.color_targets[0].texture = textures.find(
            rig.commands.get(), pass_descriptor.color_targets[0].texture->getBase()->getDescriptor(),
            TextureUsage::Write);
        pass_descriptor.depth_stencil_target.texture = textures.find(
            rig.commands.get(), pass_descriptor.depth_stencil_target.texture->getBase()->getDescriptor(),
            TextureUsage::Write);
        command.render_pass = passes.find(pass_descriptor);
        command.count = 3;
        command.state.viewports.fill({0, 0, 4, 4, 0, 1});
        command.state.scissors.fill(rig.full);
        SamplerDescriptor descriptor{};
        descriptor.depth_compare_op = engines::CompareOp::Always;
        auto* sampler = samplers.find(descriptor);
        rig.renderer.bindTexture(pass_descriptor.color_targets[0].texture, sampler,
                                 ShaderType::Fragment, 7);
        command.stages[1] = engines::ThreeDTestAccess::fragmentBindings(
            rig.engine, rig.fragment.get());
        expect(command.stages[1].reset_textures &&
                   command.stages[1].textures.size() == 1 &&
                   command.stages[1].textures[0].index == 7,
               "Absent descriptor pools snapshot inherited texture slots");
        expect(command_ir::sealDrawResources(rig.renderer, command),
               "Cached resources seal with ownership leases");
        for (const auto& owner : command.resource_owners)
            lifetimes.push_back(owner);
        expect(lifetimes.size() == 9, "Leases include shaders, pass, pipeline, sampler and both texture/view pairs");
    }
    expect(!expired() &&
               command_ir::validateDrawResources(rig.renderer, command),
           "Command leases survive cache teardown");
    expect(command_ir::sealDrawResources(rig.renderer, command),
           "Resealing preserves the previous owners while rebuilding leases");
    rig.renderer.unbindTextures(ShaderType::Fragment);
    auto copied = command;
    command = {};
    expect(command_ir::executeDraw(rig.renderer, rig.commands.get(), copied),
           "Copied command restores its explicit inherited slots");
    expect(rig.renderer.getTextureBindings(ShaderType::Fragment).size() == 1,
           "Execution restores inherited binding after renderer mutation");
    rig.renderer.unbindTextures(ShaderType::Fragment);
    rig.renderer.bindRenderPass(rig.pass.get());
    rig.renderer.bindPipeline(rig.pipeline.get());
    copied = {};
    expect(!expired(),
           "Command buffer keeps leases after packet destruction");
    rig.renderer.debugSyncCommandBuffer(rig.commands.get());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!expired() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(expired(),
           "GPU completion retires wrapper ownership");
    rig.renderer.unbindTextures(ShaderType::Fragment);
    std::cout << "PASS: Draw ownership survives caches and packets until GPU completion; inherited bindings are explicit\n";
}

void testDrawContentVersions() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    command_ir::DrawCommand command;
    command.pipeline = rig.pipeline.get();
    command.render_pass = rig.pass.get();
    command.count = 3;
    command.state.viewports.fill({0, 0, 4, 4, 0, 1});
    command.state.scissors.fill(rig.full);
    std::array<u32, 64> guest{};
    const auto base = reinterpret_cast<uptr>(guest.data());
    std::shared_ptr<BufferBase> buffer(new metal::Buffer(
        rig.renderer.getDevice()->newBuffer(256, MTL::ResourceStorageModeShared)));
    command.vertices[0] = BufferView(buffer.get())
                              .withGuestRange(ztd::Range<uptr>(base, base + 256));
    const ztd::Range<uptr> written(base + 512, base + 576);
    command.storage_writes.push_back(written);
    expect(command_ir::sealDrawResources(rig.renderer, command),
           "A draw with guest-backed buffers seals");
    expect(command.contents.reads.size() == 1 && command.contents.writes.size() >= 1,
           "The seal records what the draw reads and writes");
    expect(command_ir::validateDrawContents(rig.renderer, command),
           "Contents are current right after sealing");
    // The guest CPU rewrites the bytes behind the same buffer: identity and
    // generation are unchanged, the content is not
    const auto handle = rig.renderer.getResourceRegistry().track(
        buffer->getResourceLifetime(), resource::ResourceKind::Buffer, buffer.get());
    rig.renderer.invalidateMemory(ztd::Range<uptr>(base + 64, base + 68));
    expect(rig.renderer.getResourceRegistry().matches(
               handle, resource::ResourceKind::Buffer, buffer.get()) &&
               command_ir::validateDrawResources(rig.renderer, command),
           "A content write leaves resource identity valid");
    expect(!command_ir::validateDrawContents(rig.renderer, command),
           "A content write makes the prepared draw stale");
    expect(command_ir::sealDrawResources(rig.renderer, command) &&
               command_ir::validateDrawContents(rig.renderer, command),
           "Resealing prepares the draw against the new contents");
    // Writes elsewhere do not matter
    rig.renderer.invalidateMemory(ztd::Range<uptr>(base + 1024, base + 1028));
    expect(command_ir::validateDrawContents(rig.renderer, command),
           "Writes outside the read ranges keep the draw current");
    // Executing the draw publishes its own writes
    const auto before = rig.renderer.getContentVersions().latest(written);
    expect(command_ir::executeDraw(rig.renderer, rig.commands.get(), command),
           "The draw executes");
    expect(rig.renderer.getContentVersions().latest(written) > before,
           "Executing a draw versions the memory it wrote");
    rig.renderer.debugSyncCommandBuffer(rig.commands.get());
    const auto completed_write = rig.renderer.getContentVersions().inspect(written);
    expect(completed_write.completed == completed_write.issued &&
               !completed_write.pending && !completed_write.failed,
           "A checkpoint completes draw writes before the logical buffer is destroyed");
    std::cout << "PASS: draw content versions track guest writes separately "
                 "from resource identity\n";
}

void testClearAndCopyCommands() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    auto& versions = rig.renderer.getContentVersions();
    const auto& pass_descriptor = rig.pass->getDescriptor();
    const auto color_range =
        pass_descriptor.color_targets[0].texture->getBase()->getDescriptor().getRange();
    const auto depth_range =
        pass_descriptor.depth_stencil_target.texture->getBase()->getDescriptor().getRange();

    // Nothing to clear without a pass
    command_ir::ClearCommand empty;
    expect(command_ir::sealClear(rig.renderer, empty) &&
               command_ir::executeClear(rig.renderer, rig.commands.get(), empty),
           "A clear without a render pass succeeds trivially");

    command_ir::ClearCommand clear;
    clear.resources.capture(rig.renderer, &rig.gmmu);
    clear.render_pass = rig.pass.get();
    clear.state = {.rect = rig.full, .stencil_write_mask = 0xff};
    clear.color = command_ir::ClearCommand::Color{0, 0, 0xf, simd::uint4{1, 2, 3, 4}};
    clear.depth = command_ir::ClearCommand::Depth{0, 0.25f};
    expect(command_ir::sealClear(rig.renderer, clear) &&
               clear.resources.contents.reads.empty() &&
               clear.resources.contents.writes.size() == 2,
           "A clear reads nothing and writes the cleared attachments");
    expect(clear.resources.valid(rig.renderer),
           "A sealed clear names live resources");
    const auto color_before = versions.latest(color_range);
    const auto depth_before = versions.latest(depth_range);
    expect(command_ir::executeClear(rig.renderer, rig.commands.get(), clear),
           "A sealed clear executes");
    expect(versions.latest(color_range) > color_before &&
               versions.latest(depth_range) > depth_before,
           "Executing a clear versions the memory it wrote");

    // A colour-only clear writes only the colour target
    command_ir::ClearCommand colour_only;
    colour_only.resources.capture(rig.renderer, &rig.gmmu);
    colour_only.render_pass = rig.pass.get();
    colour_only.state = {.rect = rig.full, .stencil_write_mask = 0xff};
    colour_only.color = command_ir::ClearCommand::Color{0, 0, 0xf, simd::uint4{}};
    expect(command_ir::sealClear(rig.renderer, colour_only) &&
               colour_only.resources.contents.writes.size() == 1,
           "A colour clear writes only the colour attachment");

    // Unsealed or stale clears are rejected
    command_ir::ClearCommand unsealed = clear;
    unsealed.resources.sealed = false;
    expect(!command_ir::executeClear(rig.renderer, rig.commands.get(), unsealed),
           "An unsealed clear is rejected");
    rig.pass.reset();
    expect(!clear.resources.valid(rig.renderer) &&
               !command_ir::executeClear(rig.renderer, rig.commands.get(), clear),
           "A clear whose pass was released is rejected");

    // Copy
    std::vector<u8> memory(0x4000, 0);
    const auto base = reinterpret_cast<uptr>(memory.data());
    auto make_texture = [&](uptr ptr) {
        std::unique_ptr<ITexture> texture(rig.renderer.createTexture(TextureDescriptor(
            ptr, MTL::TextureType2D, TextureFormat::RGBA8Unorm, false, 0, 4, 4, 1,
            1, 1, 0, 0, 0)));
        return texture;
    };
    auto src_texture = make_texture(base);
    auto dst_texture = make_texture(base + 0x2000);
    const TextureViewDescriptor view_descriptor(
        MTL::TextureType2D, TextureFormat::RGBA8Unorm, {0, 1}, {0, 1});
    std::unique_ptr<ITextureView> src_view(src_texture->createView(view_descriptor));
    std::unique_ptr<ITextureView> dst_view(dst_texture->createView(view_descriptor));
    command_ir::CopyCommand copy;
    copy.resources.capture(rig.renderer, &rig.gmmu);
    copy.src = src_view.get();
    copy.dst = dst_view.get();
    copy.src_size = {4, 4, 1};
    copy.dst_size = {4, 4, 1};
    expect(command_ir::sealCopy(rig.renderer, copy) &&
               copy.resources.contents.reads.size() == 1 &&
               copy.resources.contents.writes.size() == 1,
           "A copy reads its source and writes its destination");
    const ztd::Range<uptr> src_range = src_texture->getDescriptor().getRange();
    const ztd::Range<uptr> dst_range = dst_texture->getDescriptor().getRange();
    expect(copy.resources.contentsCurrent(rig.renderer),
           "The copy's source is current when sealed");
    const auto dst_before = versions.latest(dst_range);
    expect(command_ir::executeCopy(rig.renderer, rig.commands.get(), copy) &&
               versions.latest(dst_range) > dst_before &&
               versions.latest(src_range) == 0,
           "Executing a copy versions only the destination");
    rig.renderer.invalidateMemory(ztd::Range<uptr>(src_range.getBegin(),
                                                   src_range.getBegin() + 4));
    expect(!copy.resources.contentsCurrent(rig.renderer),
           "A guest write into the source makes the prepared copy stale");
    expect(command_ir::sealCopy(rig.renderer, copy) &&
               copy.resources.contentsCurrent(rig.renderer),
           "Resealing prepares the copy against the new contents");
    command_ir::CopyCommand incomplete;
    expect(!command_ir::sealCopy(rig.renderer, incomplete),
           "A copy without textures cannot be sealed");
    dst_view.reset();
    expect(!copy.resources.valid(rig.renderer) &&
               !command_ir::executeCopy(rig.renderer, rig.commands.get(), copy),
           "A copy whose destination view was destroyed is rejected");
    rig.renderer.debugSyncCommandBuffer(rig.commands.get());
    std::cout << "PASS: clear and copy commands seal their resources, version "
                 "their writes and reject released resources\n";
}

void testCaptureAndReplay() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    auto& renderer = rig.renderer;
    const auto make_shader = [&](ShaderType type, const char* code, u32 uniform) {
        ShaderDescriptor descriptor{};
        descriptor.type = type;
        descriptor.backend = ShaderBackend::Msl;
        descriptor.code.assign(code, code + std::strlen(code));
        descriptor.resource_mapping.uniform_buffers[uniform] = uniform;
        return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
    };
    auto vertex = make_shader(ShaderType::Vertex, R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]], constant float4* pos [[buffer(0)]]) {
    return pos[id];
})", 0);
    auto fragment = make_shader(ShaderType::Fragment, R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 main_(constant float4& color [[buffer(1)]]) { return color; }
)", 1);
    PipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Vertex)] = vertex.get();
    pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Fragment)] = fragment.get();
    pipeline_descriptor.color_target_states[0].format = TextureFormat::RGBA8Unorm;
    pipeline_descriptor.color_target_states[0].write_mask = engines::ColorWriteMask::All;
    pipeline_descriptor.depth_stencil_format = TextureFormat::Z32Float;
    std::unique_ptr<PipelineBase> pipeline(renderer.createPipeline(pipeline_descriptor));

    const std::array<float, 12> positions{-1, -1, 0.5f, 1, 3, -1, 0.5f, 1,
                                          -1, 3, 0.5f, 1};
    std::unique_ptr<BufferBase> position_buffer(renderer.allocateTemporaryBuffer(sizeof(positions)));
    position_buffer->copyFrom(reinterpret_cast<uptr>(positions.data()));
    std::unique_ptr<BufferBase> color_buffer(renderer.allocateTemporaryBuffer(16));
    const auto set_color = [&](std::array<float, 4> color) {
        color_buffer->copyFrom(reinterpret_cast<uptr>(color.data()));
    };

    auto cb = rig.commands.get();
    capture::Recorder recorder(renderer);
    {
        command_ir::ClearCommand clear;
        clear.resources.capture(renderer, nullptr);
        clear.render_pass = rig.pass.get();
        clear.state = {.rect = rig.full, .stencil_write_mask = 0xff};
        const std::array<float, 4> value{0.1f, 0.2f, 0.3f, 1.0f};
        clear.color = command_ir::ClearCommand::Color{
            0, 0, 0xf, std::bit_cast<simd::uint4>(value)};
        expect(command_ir::sealClear(renderer, clear) &&
                   recorder.recordClear(cb, clear) &&
                   command_ir::executeClear(renderer, cb, clear),
               "A clear is recorded and executed");
    }
    const auto draw = [&](MTL::ScissorRect scissor, std::array<float, 4> color) {
        set_color(color);
        command_ir::DrawCommand command;
        command.pipeline = pipeline.get();
        command.render_pass = rig.pass.get();
        command.count = 3;
        command.state.viewports.fill({0, 0, 4, 4, 0, 1});
        command.state.scissors.fill(scissor);
        command.stages[0].active = true;
        command.stages[0].uniforms.push_back({0, BufferView(position_buffer.get())});
        command.stages[1].active = true;
        command.stages[1].uniforms.push_back({1, BufferView(color_buffer.get())});
        command_ir::captureDrawAddressSpace(renderer, command, nullptr);
        expect(command_ir::sealDrawResources(renderer, command) &&
                   recorder.recordDraw(cb, command) &&
                   command_ir::executeDraw(renderer, cb, command),
               "A draw is recorded and executed");
        // The CPU rewrites the shared buffer next: the GPU has to be done
        // with this draw first (the engines upload immutable snapshots)
        renderer.synchronize(cb);
    };
    // The same buffer object holds different bytes for the two draws
    draw(MTL::ScissorRect{0, 0, 3, 2}, {1, 0, 0, 1});
    draw(MTL::ScissorRect{1, 1, 3, 3}, {0, 1, 0, 1});
    auto file = recorder.finish(cb);
    expect(recorder.failure().empty(), "The capture has no failure");
    expect(file.commands.size() == 3 && file.textures.size() == 2 &&
               file.views.size() == 2 && file.shaders.size() == 2 &&
               file.pipelines.size() == 1 && file.passes.size() == 1 &&
               file.buffers.size() == 3,
           "Objects are recorded once and changed buffers get a record each");

    // What the original run drew
    const auto pixel = [&](const capture::CaptureFile& f, u32 x, u32 y) {
        const auto& blob = f.textures[0].final_contents.at(0);
        const auto* p = blob.data() + (y * 4 + x) * 4;
        return std::array<u32, 4>{p[0], p[1], p[2], p[3]};
    };
    expect(pixel(file, 0, 0) == std::array<u32, 4>({255, 0, 0, 255}),
           "Draw A covers its scissor");
    expect(pixel(file, 1, 1) == std::array<u32, 4>({0, 255, 0, 255}) &&
               pixel(file, 3, 3) == std::array<u32, 4>({0, 255, 0, 255}),
           "Draw B overwrites A and covers its own scissor");
    const auto outside = pixel(file, 3, 0);
    expect(outside[0] >= 25 && outside[0] <= 27 && outside[1] >= 50 &&
               outside[1] <= 52 && outside[2] >= 76 && outside[2] <= 78 &&
               outside[3] == 255,
           "Pixels no draw covered keep the clear");
    expect(file.textures[0].initial.size() == 1 && file.textures[1].initial.size() == 1,
           "Initial contents were read before the first command");

    // Kept for hydra-gpu-replay when asked
    if (const char* dir = std::getenv("HYDRA_TEST_CAPTURE_DIR"))
        file.save(std::filesystem::path(dir) / "two_draws.hcap");

    // Round trip through bytes
    const auto bytes = file.serialize();
    auto loaded = capture::CaptureFile::deserialize(bytes);
    expect(loaded.serialize() == bytes, "A capture survives serialization");
    {
        auto noted = loaded;
        noted.notes = {"texture a has no recorded contents", "b"};
        expect(capture::CaptureFile::deserialize(noted.serialize()).notes ==
                   noted.notes,
               "Capture notes survive serialization");
    }
    bool rejected = false;
    for (const auto& broken : {std::vector<u8>(bytes.begin(), bytes.begin() + 5),
                               std::vector<u8>(bytes.begin(), bytes.end() - 1)}) {
        try {
            (void)capture::CaptureFile::deserialize(broken);
        } catch (const std::runtime_error&) {
            rejected = true;
            continue;
        }
        rejected = false;
        break;
    }
    expect(rejected, "Truncated captures are rejected");
    auto foreign = bytes;
    foreign[0] = 'X';
    bool foreign_rejected = false;
    try {
        (void)capture::CaptureFile::deserialize(foreign);
    } catch (const std::runtime_error&) {
        foreign_rejected = true;
    }
    expect(foreign_rejected, "Files without the magic are rejected");

    // Replay on a fresh renderer reproduces the pixels
    Gpu replay_gpu;
    auto& replay_renderer = replay_gpu.getRenderer();
    const auto result = capture::replay(replay_renderer, loaded);
    expect(result.ok && result.matches,
           "Replaying the capture reproduces every written texture");
    expect(pixel(loaded, 0, 0) == std::array<u32, 4>({255, 0, 0, 255}) &&
               result.contents[0] == loaded.textures[0].final_contents,
           "The replayed pixels are those of the original draws");

    // The live path: start, the engines hand over their commands, a file is
    // written once enough were recorded, and it replays
    {
        const auto directory = std::filesystem::temp_directory_path() /
                               fmt::format("hydra_capture_test_{}", getpid());
        std::filesystem::create_directories(directory);
        auto& live = capture::LiveCapture::instance();
        live.setDirectory(directory);
        expect(!live.active(), "No capture runs before it is started");
        live.start(renderer, 2);
        expect(live.active(), "A started capture is active");
        command_ir::ClearCommand clear;
        clear.resources.capture(renderer, nullptr);
        clear.render_pass = rig.pass.get();
        clear.state = {.rect = rig.full, .stencil_write_mask = 0xff};
        clear.color = command_ir::ClearCommand::Color{0, 0, 0xf, simd::uint4{}};
        expect(command_ir::sealClear(renderer, clear), "The clear seals");
        live.clear(renderer, cb, clear);
        expect(command_ir::executeClear(renderer, cb, clear),
               "The recorded clear executes");
        live.commandExecuted(cb);
        expect(live.active(), "The capture waits for its remaining commands");
        command_ir::DrawCommand command;
        command.pipeline = pipeline.get();
        command.render_pass = rig.pass.get();
        command.count = 3;
        command.state.viewports.fill({0, 0, 4, 4, 0, 1});
        command.state.scissors.fill(rig.full);
        command.stages[0].active = true;
        command.stages[0].uniforms.push_back({0, BufferView(position_buffer.get())});
        command.stages[1].active = true;
        command.stages[1].uniforms.push_back({1, BufferView(color_buffer.get())});
        command_ir::captureDrawAddressSpace(renderer, command, nullptr);
        expect(command_ir::sealDrawResources(renderer, command), "The draw seals");
        live.draw(renderer, cb, command);
        expect(live.active(), "Recording the last command does not end the capture");
        expect(command_ir::executeDraw(renderer, cb, command),
               "The recorded draw executes");
        live.commandExecuted(cb);
        expect(!live.active(), "The capture ends once its last command ran");
        const auto path = directory / "capture_0.hcap";
        expect(std::filesystem::exists(path), "The capture file was written");
        const auto live_file = capture::CaptureFile::load(path);
        expect(live_file.commands.size() == 2, "The file holds the recorded commands");
        Gpu live_gpu;
        const auto live_result = capture::replay(live_gpu.getRenderer(), live_file);
        expect(live_result.ok && live_result.matches,
               "A capture written by the live path replays");
        std::filesystem::remove_all(directory);
        live.setDirectory({});
    }

    // Controls: changing what the capture holds changes the pixels
    auto changed = loaded;
    for (auto& buffer : changed.buffers)
        if (buffer.bytes.size() == 16 && std::bit_cast<std::array<float, 4>>(
                [&] { std::array<u8, 16> a; std::memcpy(a.data(), buffer.bytes.data(), 16); return a; }())[1] == 1.0f) {
            const std::array<float, 4> blue{0, 0, 1, 1};
            std::memcpy(buffer.bytes.data(), blue.data(), 16);
        }
    const auto changed_result = capture::replay(replay_renderer, changed);
    expect(changed_result.ok && !changed_result.matches,
           "A different buffer content gives different pixels");
    auto reordered = loaded;
    std::swap(reordered.commands[1], reordered.commands[2]);
    const auto reordered_result = capture::replay(replay_renderer, reordered);
    expect(reordered_result.ok && !reordered_result.matches,
           "A different command order gives different pixels");
    auto missing = loaded;
    missing.commands[1].draw.pipeline = 99;
    expect(!capture::replay(replay_renderer, missing).ok,
           "A capture naming a missing object fails to replay");
    std::cout << "PASS: a draw capture serializes, replays on a fresh renderer "
                 "with identical pixels and rejects damaged files\n";
}

// HYDRA_GPU_BENCH=1: reference numbers for the host cost of commands. Not a
// pass/fail test; the figures are for comparing a change against a baseline.
void benchmarkCommands() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    auto& renderer = rig.renderer;
    const auto per = [](auto&& body, u32 iterations) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < iterations; i++)
            body();
        return static_cast<double>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count()) /
               iterations;
    };

    // Content versions
    resource::ContentVersions versions;
    u32 counter = 0;
    const auto write_ns = per([&] {
        const uptr base = 0x100000 + (counter++ % 4096) * 0x100;
        versions.write(ztd::Range<uptr>(base, base + 0x80));
    }, 200000);
    const auto latest_ns = per([&] {
        const uptr base = 0x100000 + (counter++ % 4096) * 0x100;
        (void)versions.latest(ztd::Range<uptr>(base, base + 0x80));
    }, 200000);

    // A draw: seal, then encode
    command_ir::DrawCommand command;
    command.pipeline = rig.pipeline.get();
    command.render_pass = rig.pass.get();
    command.count = 3;
    command.state.viewports.fill({0, 0, 4, 4, 0, 1});
    command.state.scissors.fill(rig.full);
    std::unique_ptr<BufferBase> buffer(renderer.allocateTemporaryBuffer(256));
    std::array<u32, 8> guest{};
    const auto base = reinterpret_cast<uptr>(guest.data());
    for (u32 i = 0; i < 4; i++)
        command.vertices[i] = BufferView(buffer.get()).withGuestRange(
            ztd::Range<uptr>(base, base + 32));
    command_ir::captureDrawAddressSpace(renderer, command, nullptr);
    const auto seal_ns = per([&] {
        (void)command_ir::sealDrawResources(renderer, command);
    }, 20000);
    const auto validate_ns = per([&] {
        (void)command_ir::validateDrawContents(renderer, command);
    }, 200000);
    auto bare = command;
    for (auto& view : bare.vertices)
        view = {};
    const auto bare_seal_ns = per([&] {
        (void)command_ir::sealDrawResources(renderer, bare);
    }, 20000);
    (void)command_ir::sealDrawResources(renderer, command);
    const auto* cb = rig.commands.get();
    u32 encoded = 0;
    const auto execute_ns = per([&] {
        (void)command_ir::executeDraw(renderer, rig.commands.get(), command);
        if (++encoded % 512 == 0)
            renderer.synchronize(rig.commands.get());
    }, 4096);
    renderer.synchronize(rig.commands.get());
    (void)cb;

    // Submission
    auto* device = MTL::CreateSystemDefaultDevice();
    auto* queue = device->newCommandQueue();
    auto completions = std::make_shared<metal::CompletionQueue>();
    const auto submit_ns = per([&] {
        auto* commands = queue->commandBuffer();
        metal::commitManagedSubmission(commands, completions);
    }, 2000);
    while (completions->submissions.stats().pending != 0)
        std::this_thread::yield();
    completions.reset();
    queue->release();
    device->release();

    std::cout << fmt::format(
        "BENCH content version write {:.0f} ns, latest {:.0f} ns\n"
        "BENCH draw seal {:.0f} ns (without buffers {:.0f} ns), content check "
        "{:.0f} ns, encode {:.0f} ns\n"
        "BENCH empty managed submission {:.0f} ns\n",
        write_ns, latest_ns, seal_ns, bare_seal_ns, validate_ns, execute_ns,
        submit_ns);
}

// Buffer textures are their backing buffer: capture reads and restores its bytes
void testBufferTextureContents() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = gpu.getRenderer();
    std::unique_ptr<ITexture> texture(renderer.createTexture(TextureDescriptor(
        0, MTL::TextureTypeTextureBuffer, TextureFormat::R32Uint, true, 64, 16, 1,
        1, 1, 1, 0, 0, 0)));
    std::vector<u8> bytes(64);
    for (usize i = 0; i < bytes.size(); i++)
        bytes[i] = static_cast<u8>(i * 7 + 1);
    expect(renderer.writeTextureContents(texture.get(), {bytes}),
           "A buffer texture accepts its bytes");
    std::vector<std::vector<u8>> read;
    expect(renderer.readTextureContents(texture.get(), read) && read.size() == 1 &&
               read[0] == bytes,
           "A buffer texture returns the bytes it was given");
    expect(!renderer.writeTextureContents(texture.get(), {bytes, bytes}) &&
               !renderer.writeTextureContents(
                   texture.get(), {std::vector<u8>(1 << 20, 0)}),
           "Wrong shapes and oversized contents are refused");
    std::cout << "PASS: buffer textures are captured through their backing buffer\n";
}

void testDrawResourceValidation() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    for (u32 variant = 0; variant < 6; variant++) {
        OcclusionRig rig(32);
        command_ir::DrawCommand command;
        command.pipeline = rig.pipeline.get();
        command.render_pass = rig.pass.get();
        command.count = 3;
        command.state.viewports.fill({0, 0, 4, 4, 0, 1});
        command.state.scissors.fill(rig.full);
        command.stages[1].active = true;
        std::shared_ptr<BufferBase> borrowed_buffer(new metal::Buffer(
            rig.renderer.getDevice()->newBuffer(16, MTL::ResourceStorageModeShared)));
        command.vertices[0] = BufferView(borrowed_buffer.get());
        SamplerDescriptor sampler_descriptor{};
        sampler_descriptor.depth_compare_op = engines::CompareOp::Always;
        std::unique_ptr<SamplerBase> sampler(rig.renderer.createSampler(sampler_descriptor));
        std::unique_ptr<ITexture> texture(rig.renderer.createTexture(TextureDescriptor(
            0, MTL::TextureType2D, TextureFormat::RGBA8Unorm, false, 0,
            1, 1, 1, 1, 1, 0, 0, 0)));
        std::unique_ptr<ITextureView> view(texture->createView(TextureViewDescriptor(
            MTL::TextureType2D, TextureFormat::RGBA8Unorm, {0, 1}, {0, 1})));
        command.stages[1].textures.push_back({0, view.get(), sampler.get()});
        expect(!command_ir::validateDrawResources(rig.renderer, command),
               "Unsealed draws are rejected");
        expect(command_ir::sealDrawResources(rig.renderer, command), "Seal live resource graph");
        if (variant == 0)
            rig.pipeline.reset();
        else if (variant == 1)
            rig.pass.reset();
        else if (variant == 2)
            rig.fragment.reset();
        else if (variant == 3)
            borrowed_buffer.reset();
        else if (variant == 4)
            sampler.reset();
        else
            texture.reset(); // View is still alive; its parent is gone.
        expect(!command_ir::validateDrawResources(rig.renderer, command) &&
                   !command_ir::executeDraw(rig.renderer, rig.commands.get(), command),
               "Stale pipeline/pass/shader/buffer/sampler/texture is rejected before binding");
    }
    {
        OcclusionRig rig(32);
        command_ir::DrawCommand command;
        command.pipeline = rig.pipeline.get();
        command.render_pass = rig.pass.get();
        command.count = 3;
        expect(command_ir::sealDrawResources(rig.renderer, command), "Seal original generation");
        const auto previous = command;
        rig.pipeline->getResourceLifetime().invalidate();
        expect(!command_ir::validateDrawResources(rig.renderer, previous),
               "A changed resource generation rejects the old command");
        expect(command_ir::sealDrawResources(rig.renderer, command), "Reseal current generation");
        expect(command_ir::validateDrawResources(rig.renderer, command) &&
                   !command_ir::validateDrawResources(rig.renderer, previous),
               "Resealing does not revive an old copied command");
        std::unique_ptr<PipelineBase> replacement(rig.renderer.createPipeline(
            rig.pipeline->getDescriptor()));
        command.pipeline = replacement.get();
        expect(!command_ir::validateDrawResources(rig.renderer, command),
               "Changing a command pointer requires resealing");
    }
    {
        OcclusionRig rig(32);
        auto address_space = std::make_unique<GMmu>(nullptr);
        std::array<u32, 4> old_data{}, new_data{};
        address_space->map(0x1000, {reinterpret_cast<uptr>(old_data.data()), sizeof(old_data)});
        command_ir::DrawCommand command;
        command.pipeline = rig.pipeline.get();
        command.render_pass = rig.pass.get();
        command.count = 3;
        command_ir::captureDrawAddressSpace(rig.renderer, command, address_space.get());
        {
            resource::MappingCaptureScope capture(address_space.get(), command.mapping_dependencies);
            expect(address_space->unmapAddr(0x1000) == reinterpret_cast<uptr>(old_data.data()),
                   "Capture actual GMMU lookup dependencies");
        }
        expect(command.mapping_dependencies.size() == 1 &&
                   command_ir::sealDrawResources(rig.renderer, command),
               "Seal a command with its mapping dependencies");
        address_space->map(0x2000, {reinterpret_cast<uptr>(new_data.data()), sizeof(new_data)});
        expect(command_ir::validateDrawResources(rig.renderer, command),
               "An unrelated mapping update does not invalidate the draw");
        address_space->map(0x1000, {reinterpret_cast<uptr>(new_data.data()), sizeof(new_data)});
        expect(!command_ir::validateDrawResources(rig.renderer, command),
               "Remapping a consumed mapping rejects the old draw");
        {
            PfifoDecodeState state;
            bool notified = false;
            state.on_fault = [&] { notified = true; };
            struct RestoreDecode {
                PfifoDecodeState* previous;
                ~RestoreDecode() { tls_crnt_decode_state = previous; }
            } restore{tls_crnt_decode_state};
            tls_crnt_decode_state = &state;
            engines::ThreeDTestAccess::execute(rig.engine, command);
            expect(state.failed && state.error == 25 && notified,
                   "A rejected draw faults its channel instead of reporting success");
        }
        address_space.reset();
        expect(!command_ir::executeDraw(rig.renderer, rig.commands.get(), command),
               "Retired GMMU is rejected without dereferencing its old address");
    }
    std::cout << "PASS: command registry rejects retired resources and used mappings before encoding\n";
}

void testOcclusionReports() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(32);
    auto& renderer = rig.renderer;
    auto& engine = rig.engine;
    auto* device = renderer.getDevice();
    auto& report_memory = rig.report_memory;
    auto& gmmu = rig.gmmu;
    auto& commands = rig.commands;
    constexpr u64 REPORT = OcclusionRig::REPORT;
    const MTL::ScissorRect full = rig.full;
    renderer.setOcclusionCounterEnabled(true);
    engine.method(0x54c, 1);
    const auto draw = [&] {
        renderer.draw(commands.get(), engines::PrimitiveType::Triangles, 0, 3, 0, 1);
    };
    const auto report_range =
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(report_memory.data()),
                                   report_memory.size());
    const auto queue_report = [&](u32 counter, bool short_query = false,
                                  u64 address = REPORT) {
        engine.method(0x6c0, 0);
        engine.method(0x6c1, static_cast<u32>(address));
        engine.method(0x6c3, 2 | (counter << 23) | (static_cast<u32>(short_query) << 28));
    };
    // A report reaches memory once the GPU finished it. Whoever consumes the
    // memory in the command stream waits for that, as this does.
    const auto report = [&](u32 counter, bool short_query = false) {
        queue_report(counter, short_query);
        renderer.flushReports(commands.get(), report_range);
        u64 value = 0;
        std::memcpy(&value, report_memory.data(), short_query ? 4 : 8);
        return value;
    };

    // A report does not make the encoding thread wait for the GPU, and
    // nothing reaches memory before the GPU finished
    draw();
    std::memset(report_memory.data(), 0xa5, report_memory.size());
    const auto checkpoints = metal::CommandBuffer::getCheckpointCount();
    queue_report(0x15);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints,
           "Queueing a counter report does not wait for the GPU");
    expect(std::all_of(report_memory.begin(), report_memory.end(),
                       [](u8 value) { return value == 0xa5; }),
           "A report is not published before its commands were submitted");
    {
        // The callbacks of the submission follow its reports
        std::atomic<u64> seen_by_callback{~0ull};
        std::atomic<bool> callback_succeeded{false};
        auto finished = std::make_shared<std::promise<void>>();
        auto done = finished->get_future();
        commands->onCompletedChecked([&, finished](bool succeeded) {
            u64 value;
            std::memcpy(&value, report_memory.data(), sizeof(value));
            seen_by_callback = value;
            callback_succeeded = succeeded;
            finished->set_value();
        });
        commands.reset();
        expect(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
               "The submission completes");
        expect(seen_by_callback == 0 && callback_succeeded,
               "The callbacks of a submission run after its reports were published");
        u32 words[4];
        std::memcpy(words, report_memory.data(), sizeof(words));
        expect(words[0] == 0 && words[1] == 0 && (words[2] != 0 || words[3] != 0),
               "The published report has a zero count behind the depth clear and a timestamp");
    }
    commands.reset(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    renderer.clearDepth(commands.get(), 0, 0.25f);
    engine.method(0x54c, 1);

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

    // Conditional rendering reads the memory of a report the GPU has not
    // finished: it waits for it instead of comparing stale data
    {
        std::memset(report_memory.data(), 0, report_memory.size());
        draw();
        const auto before = metal::CommandBuffer::getCheckpointCount();
        queue_report(0x15);
        engine.method(0x554, 0);
        engine.method(0x555, static_cast<u32>(REPORT));
        engine.method(0x556, 4); // Renders if it differs from the zero reference
        expect(engine.shouldExecute(),
               "Conditional rendering sees the report that was still in flight");
        expect(metal::CommandBuffer::getCheckpointCount() > before,
               "Reading a pending report waits for the GPU");
        u64 value;
        std::memcpy(&value, report_memory.data(), sizeof(value));
        expect(value == 32, "The report that conditional rendering waited for is complete");
        // A report to other memory is not waited for
        std::array<u8, 16> other{};
        constexpr u64 OTHER = 0x410000;
        gmmu.map(OTHER, {reinterpret_cast<uptr>(other.data()), other.size()});
        queue_report(0x15, false, OTHER);
        const auto after = metal::CommandBuffer::getCheckpointCount();
        expect(engine.shouldExecute(), "The published report still differs");
        expect(metal::CommandBuffer::getCheckpointCount() == after,
               "Reading unrelated memory does not wait for pending reports");
        renderer.flushReports(commands.get(),
                              ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(other.data()), 16));
        u64 other_value;
        std::memcpy(&other_value, other.data(), sizeof(other_value));
        expect(other_value == 32, "The report to the other memory arrives");
        gmmu.unmap(OTHER);
        engine.method(0x556, 1);
        engine.method(0x54c, 1);
    }

    // An acquire of memory that a pending report writes does not run into its
    // timeout: the report is finished first
    {
        draw();
        queue_report(0x2, true);
        engine.method(0x6c2, 16);
        const auto start = std::chrono::steady_clock::now();
        engine.method(0x6c3, 1);
        expect(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500),
               "Acquiring a report of the same submission does not deadlock");
        u32 word;
        std::memcpy(&word, report_memory.data(), sizeof(word));
        expect(word == 16, "The acquired report holds the sample count");
        engine.method(0x54c, 1);
    }
    draw();
    queue_report(0x15);
    std::unique_ptr<BufferBase> indices(renderer.allocateTemporaryBuffer(6));
    const u16 triangle[] = {0, 1, 2};
    std::memcpy(reinterpret_cast<void*>(indices->getPtr()), triangle, sizeof(triangle));
    renderer.bindIndexBuffer(BufferView(indices.get()), engines::IndexType::UInt16);
    renderer.drawIndexed(commands.get(), engines::PrimitiveType::Triangles, 0, 3, 0, 0, 1);
    expect(report(0x2, true) == 32, "Indexed draws add their visible samples to the counter");
    {
        const u32 guard_word = 0xcafebabe;
        std::memcpy(report_memory.data() + 4, &guard_word, sizeof(guard_word));
        u64 timestamp_before;
        std::memcpy(&timestamp_before, report_memory.data() + 8, 8);
        expect(report(0x2, true) == 32, "A repeated report keeps the accumulated count");
        u32 kept_word;
        u64 kept_timestamp;
        std::memcpy(&kept_word, report_memory.data() + 4, 4);
        std::memcpy(&kept_timestamp, report_memory.data() + 8, 8);
        expect(kept_word == guard_word && kept_timestamp == timestamp_before,
               "Short reports leave the following words and timestamp untouched");
    }
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
           "Long-lived counters add up completed pages without losing samples");

    // Several reports of one stream: each one is the counter at its place,
    // they arrive in order, and none of them waits for the GPU
    {
        std::array<u8, 64> chain{};
        constexpr u64 CHAIN = 0x420000;
        gmmu.map(CHAIN, {reinterpret_cast<uptr>(chain.data()), chain.size()});
        engine.method(0x54c, 1);
        const auto before = metal::CommandBuffer::getCheckpointCount();
        queue_report(0x15, false, CHAIN);
        draw();
        queue_report(0x15, false, CHAIN + 16);
        draw();
        draw();
        queue_report(0x15, false, CHAIN + 32);
        engine.method(0x54c, 1);
        draw();
        queue_report(0x15, false, CHAIN + 48);
        expect(metal::CommandBuffer::getCheckpointCount() == before,
               "A stream of reports and resets never waits for the GPU");
        renderer.flushReports(commands.get(),
                              ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(chain.data()), chain.size()));
        u64 values[4], times[4];
        for (u32 i = 0; i < 4; i++) {
            std::memcpy(&values[i], chain.data() + i * 16, 8);
            std::memcpy(&times[i], chain.data() + i * 16 + 8, 8);
        }
        expect(values[0] == 0 && values[1] == 16 && values[2] == 48 && values[3] == 16,
               fmt::format("Reports hold the counter at their place in the stream "
                           "({} {} {} {})", values[0], values[1], values[2], values[3]).c_str());
        expect(times[0] <= times[1] && times[1] <= times[2] && times[2] <= times[3],
               "Reports are published in stream order");
        gmmu.unmap(CHAIN);
    }

    // A report into memory split over host mappings
    {
        std::array<u8, 4> head{};
        std::array<u8, 12> tail{};
        constexpr u64 SPLIT = 0x430000;
        gmmu.map(SPLIT, {reinterpret_cast<uptr>(head.data()), head.size()});
        gmmu.map(SPLIT + 4, {reinterpret_cast<uptr>(tail.data()), tail.size()});
        engine.method(0x54c, 1);
        draw();
        queue_report(0x15, false, SPLIT);
        renderer.flushReports(commands.get(),
                              ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(tail.data()), tail.size()));
        u32 low, high;
        u64 time;
        std::memcpy(&low, head.data(), 4);
        std::memcpy(&high, tail.data(), 4);
        std::memcpy(&time, tail.data() + 4, 8);
        expect(low == 16 && high == 0 && time != 0,
               "Counter reports span separate host mappings without losing the timestamp");
        // The memory has to exist when the report is queued
        gmmu.unmap(SPLIT + 4);
        std::memset(head.data(), 0x77, head.size());
        queue_report(0x15, false, SPLIT);
        renderer.drainReports();
        expect(std::all_of(head.begin(), head.end(), [](u8 value) { return value == 0x77; }),
               "A report whose tail is unmapped writes nothing");
        gmmu.unmap(SPLIT);
    }

    // The memory can change before the GPU produces the report. It is
    // resolved when the report is written, like the GPU would, and a report
    // into memory that is gone is dropped.
    {
        std::array<u8, 16> first{}, second{};
        constexpr u64 MOVING = 0x440000;
        gmmu.map(MOVING, {reinterpret_cast<uptr>(first.data()), first.size()});
        engine.method(0x54c, 1);
        draw();
        queue_report(0x15, false, MOVING);
        std::memset(first.data(), 0x33, first.size());
        gmmu.unmap(MOVING);
        gmmu.map(MOVING, {reinterpret_cast<uptr>(second.data()), second.size()});
        renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(first.data()), first.size()));
        u64 value;
        std::memcpy(&value, second.data(), sizeof(value));
        expect(value == 16 && first[0] == 0x33 && first[15] == 0x33,
               "A report goes to the memory mapped when the GPU produced it");
        draw();
        queue_report(0x15, false, MOVING);
        gmmu.unmap(MOVING);
        renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(second.data()), second.size()));
        renderer.drainReports();
        std::memcpy(&value, second.data(), sizeof(value));
        expect(value == 16, "A report to memory which was unmapped meanwhile is dropped");
    }

    // A stopped process receives nothing
    {
        GMmu stopped(nullptr);
        std::array<u8, 16> memory{};
        constexpr u64 STOPPED = 0x450000;
        stopped.map(STOPPED, {reinterpret_cast<uptr>(memory.data()), memory.size()});
        tls_crnt_gmmu = &stopped;
        engine.method(0x54c, 1);
        draw();
        queue_report(0x15, false, STOPPED);
        stopped.cancelGpuWork();
        renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(memory.data()), memory.size()));
        renderer.drainReports();
        expect(std::all_of(memory.begin(), memory.end(), [](u8 value) { return value == 0; }),
               "Reports of a cancelled address space are dropped");
        tls_crnt_gmmu = &gmmu;
    }

    // A failed submission publishes nothing and leaves the counter invalid
    // until it is reset. The token is the one Metal's completion would set.
    {
        std::memset(report_memory.data(), 0x5c, report_memory.size());
        engine.method(0x54c, 1);
        draw();
        static_cast<metal::CommandBuffer*>(commands.get())->getCompletionToken()->complete(false);
        std::atomic<int> outcome{-1};
        auto failed = std::make_shared<std::promise<void>>();
        auto failed_done = failed->get_future();
        queue_report(0x15);
        commands->onCompletedChecked([&, failed](bool succeeded) {
            outcome = succeeded ? 1 : 0;
            failed->set_value();
        });
        commands.reset();
        expect(failed_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
               "A failed submission still completes");
        expect(outcome == 0, "A failed submission is reported to its callbacks");
        expect(std::all_of(report_memory.begin(), report_memory.end(),
                           [](u8 value) { return value == 0x5c; }),
               "A report of a failed submission is not published");
        commands.reset(renderer.createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
        renderer.clearDepth(commands.get(), 0, 0.75f);
        draw();
        renderer.flushReports(commands.get(), report_range);
        queue_report(0x15);
        renderer.flushReports(commands.get(), report_range);
        expect(std::all_of(report_memory.begin(), report_memory.end(),
                           [](u8 value) { return value == 0x5c; }),
               "The counter stays invalid after a failed submission");
        engine.method(0x54c, 1);
        draw();
        expect(report(0x15) == 16, "A reset makes the counter valid again");

        // A report behind commands which failed has no result either, even
        // when no draw of that submission is lost
        std::memset(report_memory.data(), 0x5c, report_memory.size());
        commands.reset();
        commands.reset(renderer.createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
        static_cast<metal::CommandBuffer*>(commands.get())->getCompletionToken()->complete(false);
        queue_report(0x15);
        renderer.flushReports(commands.get(), report_range);
        expect(std::all_of(report_memory.begin(), report_memory.end(),
                           [](u8 value) { return value == 0x5c; }),
               "A report queued behind failed commands is not published");
    }

    NS_STACK_SCOPED auto witness = device->newBuffer(16, MTL::ResourceStorageModeShared);
    std::memset(witness->contents(), 0, 16);
    auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
    metal_commands->getBlitCommandEncoder()->fillBuffer(witness, NS::Range(0, 16), 0x5a);
    auto completion = std::make_shared<std::promise<void>>();
    auto finished = completion->get_future();
    commands->onCompleted([completion] { completion->set_value(); });
    engine.method(0x6c0, 0xffffff00); // Reserved upper-address bits are ignored.
    engine.method(0x6c2, 0x12345678);
    u32 previous_payload;
    std::memcpy(&previous_payload, report_memory.data(), 4);
    const auto release_checkpoints = metal::CommandBuffer::getCheckpointCount();
    engine.method(0x6c3, 1u << 28); // Short payload release.
    u32 payload;
    std::memcpy(&payload, report_memory.data(), 4);
    expect(payload == previous_payload &&
               metal::CommandBuffer::getCheckpointCount() == release_checkpoints &&
               static_cast<const u8*>(witness->contents())[0] == 0,
           "A queued release neither publishes nor blocks before its GPU work is submitted");
    renderer.flushReports(commands.get(),
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(report_memory.data()), 4));
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
    renderer.flushReports(commands.get(),
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(split_head.data()), 4));
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
    renderer.drainReports();
    // Memory written by the completion thread is invalidated by the GPU thread
    renderer.applyQueuedMemoryInvalidations();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: real occlusion counts, reset epochs, rollover and asynchronous completion-ordered reports\n";
}

// The completion queue runs work in the order it was pushed, each piece once
// its command buffer completed, whatever the order in which those complete
void testManagedMetalSubmissions() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    auto* device = MTL::CreateSystemDefaultDevice();
    auto* native_queue = device->newCommandQueue();
    auto completions = std::make_shared<metal::CompletionQueue>();
    std::weak_ptr<int> lifetime;
    std::atomic<unsigned> gpu_callbacks{0};
    std::atomic<unsigned> gpu_failures{0};
    {
        metal::CommandBuffer commands(native_queue, completions);
        auto owned = std::make_shared<int>(7);
        lifetime = owned;
        commands.retainResource(owned);
        owned.reset();
        commands.onGpuCompletedChecked([&](bool succeeded) {
            if (!succeeded)
                ++gpu_failures;
            ++gpu_callbacks;
        });
        commands.getCompletionToken();
        commands.debugCommitAndWait();
        expect(gpu_callbacks == 1,
               "GPU callbacks complete at a checkpoint instead of waiting for destruction");
        commands.onGpuCompletedChecked([&](bool succeeded) {
            if (!succeeded)
                ++gpu_failures;
            ++gpu_callbacks;
        });
        commands.getCompletionToken();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completions->submissions.stats().pending &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto stats = completions->submissions.stats();
    expect(stats.admitted == 2 && stats.succeeded == 2 && stats.pending == 0,
           "Checkpoints and destructor commits both pass through admission and retirement");
    expect(gpu_callbacks == 2 && gpu_failures == 0,
           "Each native submission runs its successful GPU callbacks once");
    expect(lifetime.expired(), "GPU completion releases retained resources");
    completions.reset();
    native_queue->release();
    device->release();
    std::cout << "PASS: managed Metal checkpoint and async submission lifecycle\n";
}

// With every admission slot held by command buffers the GPU has not started,
// a further submitter waits, and finishing the held work lets it proceed: the
// slots are freed by GPU completion alone, never by the waiting thread.
void testSubmissionSaturation() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    auto* device = MTL::CreateSystemDefaultDevice();
    auto* queue = device->newCommandQueue();
    auto* event = device->newSharedEvent();
    auto* scratch = device->newBuffer(256, MTL::ResourceStorageModeShared);
    auto completions = std::make_shared<metal::CompletionQueue>();
    const auto submit = [&](u64 wait_for) {
        auto* commands = queue->commandBuffer();
        if (wait_for != 0)
            commands->encodeWait(event, wait_for);
        auto* blit = commands->blitCommandEncoder();
        blit->fillBuffer(scratch, NS::Range(0, 256), 0x5a);
        blit->endEncoding();
        metal::commitManagedSubmission(commands, completions);
    };
    const auto wait_idle = [&](std::chrono::seconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (completions->submissions.stats().pending != 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return completions->submissions.stats().pending == 0;
    };

    for (u32 i = 0; i < 64; i++)
        submit(1);
    expect(completions->submissions.stats().pending == 64,
           "The held command buffers occupy every admission slot");
    std::atomic<bool> extra_done{false};
    std::thread extra([&] {
        submit(0);
        extra_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    expect(!extra_done && completions->submissions.stats().pending == 64,
           "A submitter beyond the budget waits instead of exceeding it");
    event->setSignaledValue(1);
    extra.join();
    expect(extra_done && wait_idle(std::chrono::seconds(10)),
           "Finishing the held work admits the waiting submitter");
    auto stats = completions->submissions.stats();
    expect(stats.admitted == 65 && stats.succeeded == 65 && stats.failed == 0 &&
               stats.peak_pending == 64,
           "Every submission completed and the budget was never exceeded");

    // Many submitters against the budget make progress and drain
    constexpr u32 THREADS = 8, PER_THREAD = 150;
    std::vector<std::thread> threads;
    for (u32 t = 0; t < THREADS; t++)
        threads.emplace_back([&] {
            for (u32 i = 0; i < PER_THREAD; i++)
                submit(0);
        });
    for (auto& thread : threads)
        thread.join();
    expect(wait_idle(std::chrono::seconds(30)),
           "Concurrent submitters drain without blocking forever");
    stats = completions->submissions.stats();
    expect(stats.admitted == 65 + THREADS * PER_THREAD &&
               stats.succeeded == stats.admitted && stats.failed == 0 &&
               stats.peak_pending <= 64,
           "All submissions succeeded within the admission budget");

    // Held work and a burst of further submitters: all complete once released
    for (u32 i = 0; i < 64; i++)
        submit(2);
    std::atomic<u32> finished{0};
    std::vector<std::thread> burst;
    for (u32 t = 0; t < 4; t++)
        burst.emplace_back([&] {
            for (u32 i = 0; i < 10; i++)
                submit(0);
            finished++;
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(finished == 0, "A burst behind held work waits");
    event->setSignaledValue(2);
    for (auto& thread : burst)
        thread.join();
    expect(wait_idle(std::chrono::seconds(30)) && finished == 4,
           "A burst behind held work completes once the work is released");
    expect(completions->submissions.stats().failed == 0 &&
               completions->submissions.stats().peak_pending <= 64,
           "The budget held through the burst");
    completions.reset();
    scratch->release();
    event->release();
    queue->release();
    device->release();
    std::cout << "PASS: Metal submission saturation holds the budget and always "
                 "makes progress\n";
}

void testCompletionQueueOrder() {
    using namespace hydra::hw::tegra_x1::gpu::renderer::metal;
    std::mutex mutex;
    std::vector<int> order;
    const auto record = [&](int value) {
        std::scoped_lock lock(mutex);
        order.push_back(value);
    };
    const auto snapshot = [&] {
        std::scoped_lock lock(mutex);
        return order;
    };
    std::array<u8, 16> memory{};
    const auto range =
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(memory.data()), memory.size());
    CompletionQueue queue;
    auto early = std::make_shared<CompletionToken>();
    auto late = std::make_shared<CompletionToken>();
    const auto first = queue.push(early, [&](bool ok) { record(ok ? 1 : -1); }, {range});
    const auto second = queue.push(late, [&](bool ok) { record(ok ? 2 : -2); });
    const auto third = queue.push(nullptr, [&](bool) { record(3); });
    expect(first < second && second < third,
           "Sequence numbers grow with the queue order");
    expect(queue.hasPendingWrites() && queue.lastWriteTo(range) == first,
           "Pending writes are found by their range");
    expect(queue.lastWriteTo(ztd::Range<uptr>::fromSize(range.getEnd(), 16)) == 0,
           "Other memory has no pending writes");
    late->complete(true);
    queue.notify();
    expect(!queue.wait(third, std::chrono::milliseconds(100)) && snapshot().empty(),
           "Work does not overtake the work pushed before it");
    early->complete(false);
    queue.notify();
    expect(queue.wait(third, std::chrono::seconds(5)), "All the work finishes");
    expect((snapshot() == std::vector<int>{-1, 2, 3}),
           "Work runs in push order and learns whether its command buffer succeeded");
    expect(!queue.hasPendingWrites() && queue.lastWriteTo(range) == 0 &&
               queue.getPendingCount() == 0,
           "Finished work has no pending writes");
    late->complete(false);
    expect(late->succeeded(), "A token completes only once");
    // What is ready when the queue goes away still runs
    std::atomic<int> ran{0};
    {
        CompletionQueue other;
        auto token = std::make_shared<CompletionToken>();
        token->complete(true);
        other.push(token, [&](bool ok) { ran = ok ? 1 : 2; });
    }
    expect(ran == 1, "Work which is ready runs before the queue is destroyed");
    std::cout << "PASS: completion queue order\n";
}

// The callbacks of a submission (its fence) follow the reports it queued, even
// slow ones, while submissions without reports are not held back
void testReportCallbackOrdering() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    std::atomic<int> publisher{0};
    std::atomic<int> seen{-1};
    renderer.queueOcclusionReport(commands.get(), [&](std::optional<u64>) {
        publisher = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        publisher = 2;
    }, {});
    commands->onCompletedChecked([&](bool succeeded) {
        seen = publisher.load() * 10 + (succeeded ? 1 : 0);
    });
    commands.reset();
    for (u32 i = 0; i < 500 && seen == -1; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(seen == 21, "The callbacks of a submission wait for the reports it queued");

    // The submissions after one with a slow report: their callbacks (their
    // fences) come after the callbacks of that one. A fence only counts, so a
    // later one reached first would satisfy the wait for the earlier one before
    // its reports were written.
    std::atomic<bool> slow_finished{false};
    std::mutex order_mutex;
    std::vector<int> order;
    std::atomic<bool> second_saw_report{false};
    commands.reset(renderer.createCommandBuffer());
    renderer.queueOcclusionReport(commands.get(), [&](std::optional<u64>) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        slow_finished = true;
    }, {});
    commands->onCompleted([&] {
        std::scoped_lock lock(order_mutex);
        order.push_back(1);
    });
    commands.reset();
    commands.reset(renderer.createCommandBuffer());
    commands->onCompleted([&] {
        second_saw_report = slow_finished.load();
        std::scoped_lock lock(order_mutex);
        order.push_back(2);
    });
    commands.reset();
    for (u32 i = 0; i < 500; i++) {
        {
            std::scoped_lock lock(order_mutex);
            if (order.size() == 2)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::scoped_lock lock(order_mutex);
        expect((order == std::vector<int>{1, 2}) && second_saw_report,
               "A submission after one with queued reports is completed after it");
    }
    // With nothing queued a submission is completed straight from Metal
    renderer.drainReports();
    std::promise<void> direct;
    auto direct_done = direct.get_future();
    commands.reset(renderer.createCommandBuffer());
    commands->onCompleted([&] { direct.set_value(); });
    commands.reset();
    expect(direct_done.wait_for(std::chrono::milliseconds(500)) ==
               std::future_status::ready,
           "A submission with no queued work in front of it completes promptly");
    std::cout << "PASS: submission callbacks follow their reports\n";
}

// A report which the GPU has not produced yet must not land on top of what
// the command stream writes to the same memory afterwards
void testReportsLandBeforeLaterWrites() {
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
    const auto range =
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(memory.data()), sizeof(memory));
    // A slow report into the first word, then a release to the same word
    renderer.queueOcclusionReport(commands.get(), [&](std::optional<u64>) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::atomic_ref<u32>(memory[0]).store(0x11111111, std::memory_order_release);
    }, {range});
    engine.method(0x6c0, 0);
    engine.method(0x6c1, static_cast<u32>(SEMAPHORE));
    engine.method(0x6c2, 0x22222222);
    engine.method(0x6c3, 1u << 28);
    renderer.flushReports(commands.get(), range);
    expect(std::atomic_ref<u32>(memory[0]).load(std::memory_order_acquire) == 0x22222222,
           "A release behind a report to the same memory has the last word");

    // The same for the memory a texture flush or a copy engine touches
    std::atomic<u32> landed{0};
    renderer.queueOcclusionReport(commands.get(), [&](std::optional<u64>) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        landed = 1;
    }, {range});
    {
        auto& cache = renderer.getTextureCache();
        std::scoped_lock lock(cache.getMutex());
        cache.flushMemory(commands.get(), range);
    }
    expect(landed == 1, "Flushing the memory of a pending report waits for the report");
    commands.reset();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: reports land before later writes to their memory\n";
}

// What tells the guest that earlier work is done (a released semaphore, a
// syncpoint, a wait for idle) comes after the reports of that work
void testSignalsFollowQueuedReports() {
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
    std::atomic<u32> landed{0};
    const auto queue_slow_report = [&] {
        landed = 0;
        renderer.queueOcclusionReport(commands.get(), [&](std::optional<u64>) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            landed = 1;
        }, {});
    };

    queue_slow_report();
    engine.method(0x6c0, 0);
    engine.method(0x6c1, static_cast<u32>(SEMAPHORE + 16));
    engine.method(0x6c2, 7);
    engine.method(0x6c3, 1u << 28);
    expect(landed == 0 && memory[4] == 0,
           "The release and its earlier reports remain pending before submission");
    renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(&memory[4]), sizeof(u32)));
    expect(landed == 1 && memory[4] == 7,
           "A released semaphore is not visible before the reports queued ahead of it");

    auto& syncpoints = gpu.getVideoSyncpoints();
    const auto syncpoint = syncpoints.allocate();
    expect(syncpoint.has_value(), "Allocate a 3D syncpoint");
    queue_slow_report();
    engine.method(0xb2, *syncpoint);
    expect(landed == 0 && syncpoints.read(*syncpoint) == 0,
           "A queued syncpoint increment does not complete before submission");
    expect(renderer.synchronize(commands.get()), "Submit work preceding the syncpoint");
    renderer.drainReports();
    expect(landed == 1 && syncpoints.read(*syncpoint) == 1,
           "A syncpoint increment is not visible before the reports queued ahead of it");

    queue_slow_report();
    gpu.subchannelMethod(0, 0x1e, 0); // WAIT_FOR_IDLE
    expect(landed == 1, "A wait for idle includes the reports queued ahead of it");

    commands.reset();
    tls_crnt_command_buffer = nullptr;
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: signals to the guest follow the queued reports\n";
}

// NVN invalidates the shader instruction cache many times per frame. Only
// commands which may have written guest memory (shader code included) are
// worth waiting for; plain GPU work is not.
void testShaderInvalidationWaitsOnlyForGuestWrites() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    auto& engine = gpu.get3DEngine();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
    // Nothing is pending from earlier tests
    renderer.synchronize(commands.get());
    NS_STACK_SCOPED auto scratch =
        renderer.getDevice()->newBuffer(64, MTL::ResourceStorageModeShared);
    metal_commands->getBlitCommandEncoder()->fillBuffer(scratch, NS::Range(0, 64), 0x11);
    const auto checkpoints = metal::CommandBuffer::getCheckpointCount();
    engine.method(0x87, 1);
    engine.method(0x369, 1);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints,
           "Invalidating shader instructions does not wait for GPU work which writes no guest memory");
    // Kernels: only one which writes a storage buffer (guest memory) is waited for
    ShaderDescriptor descriptor{};
    descriptor.type = ShaderType::Compute;
    descriptor.backend = ShaderBackend::Msl;
    const std::string code = R"(
#include <metal_stdlib>
using namespace metal;
kernel void main_(device uint* data [[buffer(0)]], uint id [[thread_position_in_grid]]) {
    data[id] = id;
})";
    descriptor.code.assign(code.begin(), code.end());
    std::unique_ptr<ShaderBase> kernel(renderer.createShader(descriptor));
    std::unique_ptr<BufferBase> storage(renderer.allocateTemporaryBuffer(64));
    ComputeDispatch dispatch{};
    dispatch.shader = kernel.get();
    dispatch.storage_buffers.push_back({0, BufferView(storage.get()), false});
    dispatch.grid_size = MTL::Size(1, 1, 1);
    dispatch.block_size = MTL::Size(16, 1, 1);
    renderer.dispatchCompute(commands.get(), dispatch);
    engine.method(0x87, 1);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints,
           "A kernel which writes no storage buffer is not waited for");
    dispatch.storage_buffers[0].written = true;
    renderer.dispatchCompute(commands.get(), dispatch);
    engine.method(0x87, 0x10);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints,
           "Only the instruction bit of the invalidation waits");
    engine.method(0x87, 1);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints + 1,
           "Invalidating shader instructions waits for kernels which write storage buffers");
    engine.method(0x87, 1);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints + 1,
           "Once the kernel finished there is nothing left to wait for");
    // A draw which writes a storage buffer counts as well (the renderer notes
    // it), also for the variant without wait for idle (NO_WFI, method 0x369)
    metal_commands->getBlitCommandEncoder()->fillBuffer(scratch, NS::Range(0, 64), 0x22);
    metal_commands->noteGuestMemoryWrites();
    engine.method(0x369, 1);
    expect(metal::CommandBuffer::getCheckpointCount() == checkpoints + 2,
           "Guest writes noted by the renderer are waited for by the NO_WFI variant too");
    commands.reset();
    tls_crnt_command_buffer = nullptr;
    std::cout << "PASS: shader invalidation waits only for GPU writes to guest memory\n";
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
        renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(memory.data()), sizeof(u32)));
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
    renderer.flushReports(commands.get(), ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(memory.data()), sizeof(u32)));
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
    {
        // Under PFIFO an unsatisfied acquire yields instead of blocking the
        // worker; the channel resumes once the word holds the value
        PfifoDecodeState state;
        tls_crnt_decode_state = &state;
        memory[0] = 0;
        engine.method(0x6c2, 9);
        const auto begin = std::chrono::steady_clock::now();
        engine.method(0x6c3, 1);
        expect(std::chrono::steady_clock::now() - begin <
                   std::chrono::milliseconds(40) &&
                   state.semaphore_wait &&
                   state.semaphore_wait->address == SEMAPHORE &&
                   state.semaphore_wait->payload == 9 &&
                   state.semaphore_wait->operation == 1,
               "An unsatisfied acquire yields with an equality wait");
        state.semaphore_wait.reset();
        engine.method(0x6c3, 1 | (1u << 16));
        expect(state.semaphore_wait && state.semaphore_wait->operation == 4,
               "An unsatisfied GE acquire yields with a >= wait");
        state.semaphore_wait.reset();
        memory[0] = 9;
        engine.method(0x6c3, 1);
        engine.method(0x6c3, 1 | (1u << 16));
        expect(!state.semaphore_wait, "A satisfied acquire does not yield");
        // Invalid semaphore words fault the channel instead of passing
        std::array<u32, 2> short_memory{};
        constexpr u64 SHORT = 0x600000;
        gmmu.map(SHORT, {reinterpret_cast<uptr>(short_memory.data()), 6});
        const auto acquire_at = [&](u64 address) {
            PfifoDecodeState probe;
            tls_crnt_decode_state = &probe;
            engine.method(0x6c0, static_cast<u32>(address >> 32));
            engine.method(0x6c1, static_cast<u32>(address));
            engine.method(0x6c3, 1);
            tls_crnt_decode_state = &state;
            return probe.failed.load() && probe.error.load() == 31 &&
                   !probe.semaphore_wait;
        };
        expect(acquire_at(0x700000), "Acquire of an unmapped address faults");
        expect(acquire_at(SHORT + 4), "Acquire of a word cut off by its mapping faults");
        expect(acquire_at(SEMAPHORE + 2), "Acquire of an unaligned word faults");
        expect(!acquire_at(SHORT), "Acquire of a valid word does not fault");
        gmmu.unmap(SHORT);
        engine.method(0x6c0, 0);
        engine.method(0x6c1, static_cast<u32>(SEMAPHORE));
        tls_crnt_decode_state = nullptr;
    }
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

    method(0x14, 0x89abcdef);
    expect(gpu.channelContext().puller_regs[0x14] == 0x89abcdef,
           "SET_REFERENCE preserves all 32 bits in channel state");

    {
        auto& metal_renderer = static_cast<renderer::metal::Renderer&>(gpu.getRenderer());
        NS_STACK_SCOPED auto reference_buffer = metal_renderer.getDevice()->newBuffer(
            16, MTL::ResourceStorageModeShared);
        std::memset(reference_buffer->contents(), 0, 16);
        std::unique_ptr<renderer::ICommandBuffer> reference_commands(
            metal_renderer.createCommandBuffer());
        static_cast<renderer::metal::CommandBuffer*>(reference_commands.get())
            ->getBlitCommandEncoder()->fillBuffer(reference_buffer, NS::Range(0, 16), 0x7b);
        tls_crnt_command_buffer = reference_commands.get();
        method(0x14, 0x12345678);
        tls_crnt_command_buffer = nullptr;
        expect(gpu.channelContext().puller_regs[0x14] == 0x12345678 &&
                   static_cast<const u8*>(reference_buffer->contents())[15] == 0x7b,
               "SET_REFERENCE publishes its value after preceding GPU work completes");
    }

    set_address(0x80);
    u32 ring_value = 0;
    std::memcpy(memory.data() + 0x80, &ring_value, sizeof(ring_value));
    method(6, 3);
    for (const u32 expected : {1u, 2u, 3u, 0u, 1u}) {
        method(7, 0x10u | (6u << 27) | (1u << 31) | (1u << 24));
        std::memcpy(&ring_value, memory.data() + 0x80, sizeof(ring_value));
        expect(ring_value == expected, "Unsigned semaphore INC cycles through the inclusive limit");
    }
    ring_value = 0;
    std::memcpy(memory.data() + 0x80, &ring_value, sizeof(ring_value));
    for (const u32 expected : {3u, 2u, 1u, 0u, 3u}) {
        method(7, 0x10u | (7u << 27) | (1u << 31) | (1u << 24));
        std::memcpy(&ring_value, memory.data() + 0x80, sizeof(ring_value));
        expect(ring_value == expected, "Unsigned semaphore DEC cycles down through zero");
    }
    for (const u32 operation : {6u, 7u}) {
        PfifoDecodeState invalid;
        tls_crnt_decode_state = &invalid;
        method(7, 0x10u | (operation << 27) | (1u << 24));
        tls_crnt_decode_state = nullptr;
        u32 unchanged;
        std::memcpy(&unchanged, memory.data() + 0x80, sizeof(unchanged));
        expect(invalid.failed && invalid.error == 25 && unchanged == ring_value,
               "Unsupported signed INC/DEC faults without changing the semaphore");
    }

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
        expect(syncpoints.read(*engine_syncpoint) == expected_count - 1,
               "A 3D syncpoint remains pending until its GPU commands complete");
        expect(gpu.getRenderer().synchronize(commands.get()),
               "Submit GPU work preceding the 3D syncpoint");
        gpu.getRenderer().drainReports();
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

// Random programs of draws, reports, resets and submissions, written by
// tests/occlusion_reference.py, run on Metal: the reports of each program are
// compared with an independent model of the counter. One line per program,
// operations separated by spaces:
//   D<n>   n draws (counted by the coverage and the counting state)
//   C0/C1/C2   coverage of a draw: 16 samples, none (depth), 4 (scissor)
//   X0/X1  counting off/on
//   R<s>/r<s>   long/short report into slot s
//   Z      reset of the counter     S  submit (new command buffer)
//   E      end the render encoder   F  wait for the pending reports
int runOcclusionCases(const char* input_path, const char* output_path) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    constexpr u32 SLOTS = 48;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    OcclusionRig rig(SLOTS * 16);
    auto& renderer = rig.renderer;
    auto& engine = rig.engine;
    auto& commands = rig.commands;
    const auto memory = ztd::Range<uptr>::fromSize(
        reinterpret_cast<uptr>(rig.report_memory.data()), rig.report_memory.size());
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    if (!input || !output) {
        std::cerr << "Cannot open the case files\n";
        return 2;
    }
    std::string line;
    u32 program = 0;
    while (std::getline(input, line)) {
        // Every program starts from the same state
        std::fill(rig.report_memory.begin(), rig.report_memory.end(), 0);
        rig.setScissor(rig.full);
        renderer.clearDepth(commands.get(), 0, 0.75f);
        renderer.setOcclusionCounterEnabled(true);
        engine.method(0x54c, 1);
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            const char kind = token[0];
            const u32 number = token.size() > 1
                                   ? static_cast<u32>(std::stoul(token.substr(1)))
                                   : 0;
            switch (kind) {
            case 'D':
                for (u32 i = 0; i < std::max(1u, number); i++)
                    renderer.draw(commands.get(), engines::PrimitiveType::Triangles,
                                  0, 3, 0, 1);
                break;
            case 'C':
                rig.setScissor(number == 2 ? MTL::ScissorRect(0, 0, 2, 2) : rig.full);
                renderer.clearDepth(commands.get(), 0, number == 1 ? 0.25f : 0.75f);
                break;
            case 'X':
                renderer.setOcclusionCounterEnabled(number != 0);
                break;
            case 'R':
            case 'r':
                engine.method(0x6c0, 0);
                engine.method(0x6c1, static_cast<u32>(OcclusionRig::REPORT + number * 16));
                engine.method(0x6c3, 2 | (0x15u << 23) | (kind == 'r' ? 1u << 28 : 0u));
                break;
            case 'Z':
                engine.method(0x54c, 1);
                break;
            case 'S':
                commands.reset(renderer.createCommandBuffer());
                tls_crnt_command_buffer = commands.get();
                break;
            case 'E':
                static_cast<metal::CommandBuffer*>(commands.get())->getBlitCommandEncoder();
                break;
            case 'F':
                renderer.flushReports(commands.get(), memory);
                break;
            default:
                std::cerr << "Unknown operation " << token << '\n';
                return 2;
            }
        }
        renderer.flushReports(commands.get(), memory);
        output << "P " << program++ << '\n';
        for (u32 slot = 0; slot < SLOTS; slot++) {
            u64 value, timestamp;
            std::memcpy(&value, rig.report_memory.data() + slot * 16, sizeof(value));
            std::memcpy(&timestamp, rig.report_memory.data() + slot * 16 + 8,
                        sizeof(timestamp));
            output << "S " << slot << ' ' << value << ' ' << timestamp << '\n';
        }
    }
    renderer.drainReports();
    return 0;
}

int runSemaphoreCases(const char* input_path, const char* output_path) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    auto* device = renderer.getDevice();
    NS::Error* error = nullptr;
    const char* source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void initialize(device uint* memory [[buffer(0)]],
                       constant uint& initial [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    memory[i] = i == 4 ? initial : 0xa5c39e71u;
}
kernel void observe(const device uint* memory [[buffer(0)]],
                    device uint* results [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    results[i] = memory[i];
})";
    NS_STACK_SCOPED auto library = device->newLibrary(metal::toNsString(source), nullptr, &error);
    expect(library != nullptr, "Semaphore reference kernels compile");
    NS_STACK_SCOPED auto initialize = library->newFunction(metal::toNsString("initialize"));
    NS_STACK_SCOPED auto observe = library->newFunction(metal::toNsString("observe"));
    NS_STACK_SCOPED auto initialize_pipeline = device->newComputePipelineState(initialize, &error);
    NS_STACK_SCOPED auto observe_pipeline = device->newComputePipelineState(observe, &error);
    expect(initialize_pipeline && observe_pipeline, "Semaphore reference pipelines compile");
    NS_STACK_SCOPED auto memory = device->newBuffer(4096, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto results = device->newBuffer(32, MTL::ResourceStorageModeShared);
    GMmu gmmu(nullptr);
    constexpr u64 ADDRESS = 0x710000;
    gmmu.map(ADDRESS, {reinterpret_cast<uptr>(memory->contents()), 4096});
    tls_crnt_gmmu = &gmmu;
    struct ResetTls {
        ~ResetTls() {
            tls_crnt_gmmu = nullptr;
            tls_crnt_decode_state = nullptr;
            tls_crnt_command_buffer = nullptr;
        }
    } reset_tls;
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    expect(bool(input) && bool(output), "Open semaphore case files");
    u32 operation, signed_format, initial, payload, repetitions, no_wfi;
    u32 count = 0;
    while (input >> operation >> signed_format >> initial >> payload >> repetitions >> no_wfi) {
        expect(operation < 16 && signed_format < 2 && no_wfi < 2 &&
                   repetitions >= 1 && repetitions <= 1024, "Valid semaphore fixture");
        PfifoDecodeState decode;
        tls_crnt_decode_state = &decode;
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
        auto* native = static_cast<metal::CommandBuffer*>(commands.get());
        auto* encoder = native->getComputeCommandEncoder();
        encoder->setComputePipelineState(initialize_pipeline);
        encoder->setBuffer(memory, 0, 0);
        encoder->setBytes(&initial, sizeof(initial), 1);
        encoder->dispatchThreads(MTL::Size(8, 1, 1), MTL::Size(8, 1, 1));
        if (no_wfi)
            expect(renderer.synchronize(commands.get()), "Initialize no-WFI fixture on GPU");
        const auto before = metal::CommandBuffer::getCheckpointCount();
        const auto method = [&](u32 index, u32 value) { gpu.subchannelMethod(0, index, value); };
        method(4, 0);
        method(5, static_cast<u32>(ADDRESS + 16));
        method(6, payload);
        const u32 control = 0x10u | (operation << 27) |
            ((signed_format == 0 ? 1u : 0u) << 31) | (no_wfi << 20) | (1u << 24);
        for (u32 step = 0; step < repetitions; ++step)
            method(7, control);
        const auto checkpoints = metal::CommandBuffer::getCheckpointCount() - before;
        encoder = native->getComputeCommandEncoder();
        encoder->setComputePipelineState(observe_pipeline);
        encoder->setBuffer(memory, 0, 0);
        encoder->setBuffer(results, 0, 1);
        encoder->dispatchThreads(MTL::Size(8, 1, 1), MTL::Size(8, 1, 1));
        expect(renderer.synchronize(commands.get()), "Observe semaphore on actual Metal GPU");
        const auto* words = static_cast<const u32*>(results->contents());
        for (u32 i = 0; i < 8; ++i)
            if (i != 4) expect(words[i] == 0xa5c39e71u, "Semaphore reduction preserves neighboring GPU words");
        output << words[4] << ' ' << decode.error.load() << ' ' << checkpoints << '\n';
        tls_crnt_decode_state = nullptr;
        tls_crnt_command_buffer = nullptr;
        ++count;
    }
    expect(input.eof(), "Parse semaphore cases");
    std::cout << "PASS: " << count << " production puller reductions with Metal initialization/readback\n";
    return 0;
}

int runChannelMethodCases(const char* input_path, const char* output_path) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
    NS_STACK_SCOPED auto memory = renderer.getDevice()->newBuffer(4096, MTL::ResourceStorageModeShared);
    struct ResetTls {
        ~ResetTls() {
            tls_crnt_decode_state = nullptr;
            tls_crnt_command_buffer = nullptr;
        }
    } reset_tls;
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    expect(bool(input) && bool(output), "Open channel method cases");
    u32 method, argument, fill;
    while (input >> method >> argument >> fill) {
        expect(method < 0x1000 && fill <= 255, "Valid channel method fixture");
        PfifoDecodeState decode;
        decode.context = std::make_unique<GpuChannelContext>(gpu);
        tls_crnt_decode_state = &decode;
        std::atomic<bool> published{false};
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
        auto* native = static_cast<metal::CommandBuffer*>(commands.get());
        native->getBlitCommandEncoder()->fillBuffer(memory, NS::Range(0, 4096), static_cast<u8>(fill));
        renderer.queueGuestWrite(commands.get(), [&published](bool succeeded) {
            published.store(succeeded, std::memory_order_release);
        }, {});
        const auto before = metal::CommandBuffer::getCheckpointCount();
        gpu.subchannelMethod(0, method, argument);
        const auto checkpoints = metal::CommandBuffer::getCheckpointCount() - before;
        const bool published_before = published.load(std::memory_order_acquire);
        u32 visible = 0;
        if (published_before)
            std::memcpy(&visible, memory->contents(), sizeof(visible));
        expect(renderer.synchronize(commands.get()), "Complete real Metal work after channel method");
        renderer.drainReports();
        const auto* bytes = static_cast<const u8*>(memory->contents());
        expect(published.load(std::memory_order_acquire) &&
                   std::all_of(bytes, bytes + 4096, [fill](u8 byte) { return byte == fill; }),
               "Prior Metal work and its ordered publisher complete");
        output << decode.error.load() << ' ' << checkpoints << ' '
               << published_before << ' ' << visible << '\n';
        tls_crnt_decode_state = nullptr;
        tls_crnt_command_buffer = nullptr;
    }
    expect(input.eof(), "Parse channel method cases");
    return 0;
}

void testComputeCommandsAndCapture() {
    using namespace renderer;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    metal::Renderer renderer;
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    ShaderDescriptor descriptor{};
    descriptor.type = ShaderType::Compute;
    descriptor.backend = ShaderBackend::Msl;
    const std::string code = R"(
#include <metal_stdlib>
using namespace metal;
kernel void main_(device uint* values [[buffer(0)]], device uint* alias [[buffer(1)]]) {
    values[0] += 1;
    alias[0] = values[0] + 5;
})";
    descriptor.code.assign(code.begin(), code.end());
    std::shared_ptr<ShaderBase> kernel(renderer.createShader(descriptor));
    kernel->setResourceOwner(kernel);
    std::shared_ptr<BufferBase> storage(renderer.allocateTemporaryBuffer(64));
    std::array<u32, 16> zeros{};
    storage->copyFrom(reinterpret_cast<uptr>(zeros.data()));
    const auto ptr = storage->getPtr();
    command_ir::ComputeCommand compute;
    compute.dispatch.shader = kernel.get();
    compute.dispatch.grid_size = {1, 1, 1};
    compute.dispatch.block_size = {1, 1, 1};
    compute.dispatch.storage_buffers.push_back(
        {0, BufferView(storage).withGuestRange({ptr, ptr + 64}), true});
    compute.dispatch.storage_buffers.push_back(
        {1, BufferView(storage, 4, 60).withGuestRange({ptr + 4, ptr + 64}),
         true});
    expect(command_ir::sealCompute(renderer, compute),
           "Compute resources seal");
    expect(compute.resources.contents.reads.size() == 2 &&
               compute.resources.contents.writes.size() == 2,
           "Compute records overlapping storage reads and writes");
    compute.dispatch.storage_buffers[0].index = 3;
    expect(!command_ir::executeCompute(renderer, commands.get(), compute),
           "A binding changed after sealing cannot execute");
    compute.dispatch.storage_buffers[0].index = 0;
    compute.dispatch.block_size.width = 2;
    expect(!command_ir::validateCompute(renderer, compute),
           "Dispatch shape is sealed");
    compute.dispatch.block_size.width = 1;
    expect(command_ir::validateCompute(renderer, compute),
           "Restored bindings validate");
    compute.resources.origin = {7, 11, 0x1000};
    auto empty = compute;
    empty.dispatch.grid_size.width = 0;
    const auto before = renderer.getContentVersions().current();
    expect(command_ir::sealCompute(renderer, empty) &&
               command_ir::executeCompute(renderer, commands.get(), empty) &&
               renderer.getContentVersions().current() == before,
           "A zero-sized dispatch succeeds without publishing writes");
    auto invalid = compute;
    invalid.dispatch.storage_buffers[0].buffer = BufferView(storage, 63, 8);
    expect(!command_ir::sealCompute(renderer, invalid),
           "Compute rejects a buffer range outside its allocation");
    auto invalid_binding = compute;
    invalid_binding.dispatch.storage_buffers[0].index = 100;
    expect(command_ir::sealCompute(renderer, invalid_binding) &&
               !command_ir::executeCompute(renderer, commands.get(),
                                           invalid_binding) &&
               renderer.getContentVersions().current() == before,
           "Backend binding failure does not publish a successful GPU write");

    capture::Recorder recorder(renderer);
    expect(recorder.recordCompute(commands.get(), compute),
           "Compute enters capture");
    kernel.reset();
    expect(command_ir::executeCompute(renderer, commands.get(), compute),
           "The sealed command retains its shader after caller release");
    recorder.commandExecuted(commands.get());
    std::vector<u8> actual;
    expect(renderer.readBufferContents(BufferView(storage), actual),
           "Read actual compute output");
    std::array<u32, 2> values{};
    std::memcpy(values.data(), actual.data(), sizeof(values));
    expect(values[0] == 1 && values[1] == 6,
           "Compute executes exactly once and bindings preserve shared backing "
           "and offsets");

    auto source = std::unique_ptr<ITexture>(renderer.createTexture(
        TextureDescriptor(0, MTL::TextureType2D, TextureFormat::RGBA8Unorm,
                          false, 0, 4, 4, 1, 1, 1, 0, 0, 0)));
    auto destination = std::unique_ptr<ITexture>(
        renderer.createTexture(source->getDescriptor()));
    const capture::Contents pixels{std::vector<u8>(64, 0x5a)};
    expect(renderer.writeTextureContents(source.get(), pixels) &&
               renderer.writeTextureContents(destination.get(),
                                             {std::vector<u8>(64, 0)}),
           "Initialize copy capture textures");
    TextureViewDescriptor view_descriptor(
        MTL::TextureType2D, TextureFormat::RGBA8Unorm, {0, 1}, {0, 1});
    std::unique_ptr<ITextureView> src(source->createView(view_descriptor));
    std::unique_ptr<ITextureView> dst(destination->createView(view_descriptor));
    command_ir::CopyCommand copy;
    copy.src = src.get();
    copy.dst = dst.get();
    copy.src_size = {4, 4, 1};
    copy.dst_size = {4, 4, 1};
    expect(command_ir::sealCopy(renderer, copy) &&
               recorder.recordCopy(commands.get(), copy) &&
               command_ir::executeCopy(renderer, commands.get(), copy),
           "A texture copy enters the same ordered capture as compute");
    auto file = recorder.finish(commands.get());
    expect(recorder.failure().empty() && file.commands.size() == 2 &&
               file.commands[0].kind == capture::CommandRecord::Kind::Compute &&
               file.commands[1].kind == capture::CommandRecord::Kind::Copy,
           "Capture preserves mixed command order");
    const auto& captured_storage = file.commands[0].compute.storage;
    expect(
        captured_storage[0].buffer.buffer ==
                captured_storage[1].buffer.buffer &&
            captured_storage[1].buffer.offset == 4 &&
            !captured_storage[0].expected.empty(),
        "Capture preserves storage aliases, offsets and per-command results");
    auto decoded = capture::CaptureFile::deserialize(file.serialize());
    expect(decoded.commands[0].origin.channel == 7 &&
               decoded.commands[0].origin.sequence == 11 &&
               decoded.commands[0].origin.source_address == 0x1000,
           "Guest command origin survives capture serialization");
    metal::Renderer replay_renderer;
    const auto replayed = capture::replay(replay_renderer, decoded);
    expect(replayed.ok && replayed.matches,
           "Mixed compute/copy replay matches actual GPU results");
    decoded.commands[0].compute.storage[0].expected[0] ^= 1;
    const auto mismatch = capture::replay(replay_renderer, decoded);
    expect(
        mismatch.ok && !mismatch.matches && !mismatch.mismatch.empty(),
        "Storage result corruption is detected even when final textures match");
    decoded = file;
    decoded.commands[0].compute.storage[0].buffer.offset =
        std::numeric_limits<u64>::max();
    expect(!capture::replay(replay_renderer, decoded).ok,
           "Malformed captured buffer offsets are rejected");
    auto legacy = capture::CaptureFile{}.serialize();
    const u32 old_version = 3;
    std::memcpy(legacy.data() + sizeof(capture::MAGIC), &old_version,
                sizeof(old_version));
    expect(capture::CaptureFile::deserialize(legacy).commands.empty(),
           "Version 3 captures remain readable");
    std::cout << "PASS: compute IR validation, leases, no-op, failure "
                 "propagation, aliases and mixed capture/replay\n";
}

// The kinds of GPU thread work are exclusive and add up to the root's time
void testGpuWorkTracker() {
    using namespace std::chrono;
    using namespace hydra::hw::tegra_x1::gpu;
    auto& stats = PerfStats::instance();
    for (auto& ns : stats.gpu_work_ns)
        ns = 0;
    const auto spin = [](microseconds duration) {
        const auto end = steady_clock::now() + duration;
        while (steady_clock::now() < end) {
        }
    };
    {
        // Without a root (another thread, or stats disabled) nothing counts
        ScopedGpuWork outside(GpuWork::Encode);
        spin(milliseconds(2));
    }
    steady_clock::time_point begin, end;
    {
        begin = steady_clock::now();
        ScopedGpuWorkRoot root;
        spin(milliseconds(2)); // Decode
        {
            ScopedGpuWork lookup(GpuWork::Lookup);
            spin(milliseconds(2));
            {
                ScopedGpuWork shader(GpuWork::Shader);
                spin(milliseconds(4));
            }
            spin(milliseconds(2));
        }
        {
            ScopedGpuWork wait(GpuWork::Wait);
            spin(milliseconds(2));
        }
        // Back to decode after the nested scopes
        spin(milliseconds(1));
        end = steady_clock::now();
    }
    const auto ms = [&](GpuWork work) {
        return static_cast<double>(stats.gpu_work_ns[static_cast<usize>(work)]) /
               1e6;
    };
    const auto near = [](double value, double expected) {
        return value >= expected && value < expected + 1.0;
    };
    double total = 0;
    for (usize i = 0; i < GPU_WORK_COUNT; i++)
        total += ms(static_cast<GpuWork>(i));
    const double elapsed =
        duration<double, std::milli>(end - begin).count();
    expect(ms(GpuWork::Encode) == 0.0, "Work outside a root was counted");
    expect(near(ms(GpuWork::Decode), 3.0), "Root time outside scopes is decode");
    expect(near(ms(GpuWork::Lookup), 4.0), "Nested time was not subtracted");
    expect(near(ms(GpuWork::Shader), 4.0), "Nested scope time is wrong");
    expect(near(ms(GpuWork::Wait), 2.0), "Wait time is wrong");
    expect(total >= elapsed - 0.05 && total <= elapsed + 0.5,
           "Kinds do not add up to the root's time");
    expect(isGpuWorkAsyncCapable(GpuWork::Shader) &&
               !isGpuWorkAsyncCapable(GpuWork::Lookup) &&
               !isGpuWorkAsyncCapable(GpuWork::Wait),
           "Wrong sync/async classification");
    std::cout << "PASS: GPU thread work kinds are exclusive (total "
              << total << " ms of " << elapsed << " ms)\n";
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--gpu-work-tracker") {
        // Statistics are only collected with HYDRA_PERF_STATS
        expect(hydra::hw::tegra_x1::gpu::PerfStats::isEnabled(),
               "Run with HYDRA_PERF_STATS=1");
        testGpuWorkTracker();
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--check-optimization-policy") {
        const auto* requested = std::getenv("HYDRA_GPU_OPTIMIZATIONS");
        const bool expected = requested && std::string_view(requested) == "1";
        if (optimization::enabled() != expected) {
            std::cerr << "FAIL: optimization receipt did not select the "
                         "requested policy\n";
            return 1;
        }
        return 0;
    }
    if (argc == 2 &&
        (std::string_view(argv[1]) == "--check-compute-ir" ||
         std::string_view(argv[1]) == "--check-compute-ir-profile")) {
        try {
            if (std::string_view(argv[1]) == "--check-compute-ir-profile")
                setenv("HYDRA_PROFILE_COMPUTE", "1", 1);
            testComputeCommandsAndCapture();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--channel-method-cases") {
        try {
            return runChannelMethodCases(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--semaphore-cases") {
        try {
            return runSemaphoreCases(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::string(argv[1]) == "--occlusion-cases") {
        try {
            return runOcclusionCases(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
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
        testDrawStateSnapshots();
        testDrawCommands();
        testDrawResourceValidation();
        testDrawContentVersions();
        testClearAndCopyCommands();
        testComputeCommandsAndCapture();
        testCaptureAndReplay();
        testBufferTextureContents();
        if (std::getenv("HYDRA_GPU_BENCH"))
            benchmarkCommands();
        testDrawResourceLeases();
        testRenderTargetMappingMemo();
        testOcclusionReports();
        testCompletionQueueOrder();
        testSubmissionSaturation();
        testManagedMetalSubmissions();
        testReportCallbackOrdering();
        testReportsLandBeforeLaterWrites();
        testSignalsFollowQueuedReports();
        testShaderInvalidationWaitsOnlyForGuestWrites();
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

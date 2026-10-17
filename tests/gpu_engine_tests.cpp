// Engine-level GPU tests: the macro (MME) interpreter and renderer operations
// the engines issue (clears), checked on Metal where pixels are involved.
#include "core/hw/tegra_x1/gpu/macro/interpreter/driver.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/engines/2d.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
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
                             TextureDescriptor(ptr, TextureType::_2D,
                                               TextureFormat::RGBA8Unorm, false,
                                               0, 8, 8, 1, 1, 1, 0, 0, 0),
                             TextureUsage::Write);
    auto* depth = cache.find(commands.get(),
                             TextureDescriptor(ptr + 0x10000, TextureType::_2D,
                                               TextureFormat::Z32Float, false, 0,
                                               8, 8, 1, 1, 1, 0, 0, 0),
                             TextureUsage::Write);
    RenderPassDescriptor pass_descriptor{};
    pass_descriptor.color_targets[0].texture = color;
    pass_descriptor.depth_stencil_target.texture = depth;
    std::unique_ptr<RenderPassBase> pass(
        renderer.createRenderPass(pass_descriptor));
    renderer.bindRenderPass(pass.get());
    const Scissor full(uint2({0, 0}), uint2({8, 8}));
    renderer.setClearState({.rect = full});
    renderer.clearColor(commands.get(), 0, 0, 0xf, uint4({0, 0, 0, 0}));
    renderer.clearDepth(commands.get(), 0, 0.75f);
    // Partial color clear right after the depth clear
    renderer.setClearState({.rect = Scissor(uint2({2, 1}), uint2({4, 2}))});
    renderer.clearColor(commands.get(), 0, 0, 0xf,
                        uint4({std::bit_cast<u32>(1.0f), 0,
                               std::bit_cast<u32>(1.0f),
                               std::bit_cast<u32>(1.0f)}));
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
    const auto first = decode(engines::PrimitiveType::Triangles, 8, type,
                              primitive, count);
    expect(type == engines::IndexType::UInt16 && count == 8,
           "U8 indices decode to U16");
    expect(read_u16(first, 8) == std::vector<u16>({3, 1, 4, 1, 5, 9, 2, 6}),
           "U8 indices are widened");
    expect(decode(engines::PrimitiveType::Triangles, 8, type, primitive, count)
                   .getBase() == first.getBase(),
           "Unchanged indices reuse their decode");

    indices[2] = 42;
    const auto rewritten = decode(engines::PrimitiveType::Triangles, 8, type,
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
    expect(primitive == engines::PrimitiveType::LineStrip && count == 4 &&
               read_u16(loop, 4) == std::vector<u16>({3, 1, 42, 3}),
           "Line loops are closed line strips");
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

    // Acquire: satisfied at once, or abandoned after a bounded wait
    set_address(0x10);
    const auto begin = std::chrono::steady_clock::now();
    method(0x1a, 0xabcd0001u);
    expect(std::chrono::steady_clock::now() - begin <
               std::chrono::milliseconds(50),
           "An acquired semaphore that holds the value does not wait");
    method(0x1a, 0x12345678u);
    const auto waited = std::chrono::steady_clock::now() - begin;
    expect(waited < std::chrono::seconds(2),
           "An acquire that is never released stops waiting");

    // Another thread releases an awaited semaphore
    set_address(0x30);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const u32 value = 5;
        std::memcpy(memory.data() + 0x30, &value, 4);
    });
    method(0x6, 5);
    method(0x7, 0x1);
    releaser.join();

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

    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: channel semaphores and GPU fences\n";
}

} // namespace

int main() {
    try {
        testMacroAlu();
        testMacroBitfields();
        testMacroControlFlow();
        testMacroParametersAndNesting();
        testMacroResultOperations();
        testRendererClears();
        testIndexDecoding();
        testCacheCollisions();
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

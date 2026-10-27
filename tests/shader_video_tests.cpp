// Maxwell video integer instruction regressions, decoded and executed on Metal.
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/move.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
using hydra::hw::tegra_x1::gpu::renderer::metal::toNsString;

// Runs the blocks of main, each returns the next one
static std::string runProgramSource(const ir::Module& module) {
    std::string source =
        "\ninline void run_program(thread State& state) { auto next = "
        "Block_main::label0x0; uint guard=0; while(next != Block_main::None && "
        "guard++ < 4096u) { switch(next) {";
    for (const auto& [label, block] :
         const_cast<ir::Module&>(module).getFunction("main").getBlocks()) {
        (void)block;
        source += fmt::format(
            "case Block_main::{}: next=func_{}(state); break;", label, label);
    }
    source += "default: return; } } }\n";
    return source;
}

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
MTL::Library* compileLibrary(MTL::Device* device, const std::string& source,
                             const char* what) {
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
    options->setFastMathEnabled(false);
    auto* library = device->newLibrary(toNsString(source), options, &error);
    if (library == nullptr) {
        if (error != nullptr)
            std::cerr << error->localizedDescription()->utf8String() << '\n';
        std::cerr << source << '\n';
    }
    expect(library != nullptr, what);
    return library;
}

struct AluState {
    std::array<u32, 32> regs{};
    u32 preds{0};
    u32 cc{0};
};

std::vector<AluState> runAlu(MTL::Device* device, const std::vector<u64>& insts,
                             const std::vector<AluState>& inputs,
                             const std::vector<u32>& cbuf, u32 cbuf_slot,
                             const char* what, u64 code_offset = 0) {
    std::vector<u64> words;
    for (const auto inst : insts) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(inst);
    }
    if (words.size() % 4 == 0)
        words.push_back(0);
    words.push_back(0xe30000000007000full); // EXIT
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder, code_offset});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState state{};
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, state, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += runProgramSource(module);

    std::string cbuf_assignments;
    for (const auto slot : memory.getConstBuffers()) {
        expect(slot == cbuf_slot, "Test program reads one constant buffer");
        cbuf_assignments += fmt::format("state.c{} = cb;", slot);
    }
    source += fmt::format(R"(
kernel void run_alu(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]],
                    constant Reg* cb [[buffer(2)]], uint tid [[thread_position_in_grid]]) {{
    State state;
    constant uint* src = in + tid * 34u;
    for (uint i = 0; i < 32; i++) state.r[i]._u32 = src[i];
    for (uint i = 0; i < 8; i++) state.p[i] = ((src[32] >> i) & 1u) != 0u;
    state.p[7] = true;
    for (uint i = 0; i < 4; i++) state.cc[i] = ((src[33] >> i) & 1u) != 0u;
    {}
    run_program(state);
    device uint* dst = out + tid * 34u;
    for (uint i = 0; i < 32; i++) dst[i] = state.r[i]._u32;
    uint preds = 0u, cc = 0u;
    for (uint i = 0; i < 7; i++) preds |= state.p[i] ? (1u << i) : 0u;
    for (uint i = 0; i < 4; i++) cc |= state.cc[i] ? (1u << i) : 0u;
    dst[32] = preds;
    dst[33] = cc;
}}
)",
                          cbuf_assignments);
    NS_STACK_SCOPED auto library = compileLibrary(device, source, what);
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("run_alu"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, what);
    std::vector<u32> input_words;
    for (const auto& input : inputs) {
        input_words.insert(input_words.end(), input.regs.begin(),
                           input.regs.end());
        input_words.push_back(input.preds);
        input_words.push_back(input.cc);
    }
    std::vector<u32> cbuf_words = cbuf;
    cbuf_words.resize(std::max<usize>(cbuf_words.size(), 64));
    NS_STACK_SCOPED auto in =
        device->newBuffer(input_words.data(), input_words.size() * 4,
                          MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto cb =
        device->newBuffer(cbuf_words.data(), cbuf_words.size() * 4,
                          MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto out = device->newBuffer(
        input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(out, 0, 0);
    encoder->setBuffer(in, 0, 1);
    encoder->setBuffer(cb, 0, 2);
    encoder->dispatchThreads(MTL::Size(inputs.size(), 1, 1),
                             MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    std::vector<AluState> results(inputs.size());
    const auto* words_out = static_cast<const u32*>(out->contents());
    for (usize i = 0; i < inputs.size(); ++i) {
        std::memcpy(results[i].regs.data(), words_out + i * 34, 32 * 4);
        results[i].preds = words_out[i * 34 + 32];
        results[i].cc = words_out[i * 34 + 33];
    }
    return results;
}

constexpr u64 ALWAYS = 7ull << 16;

u32 extendVideo(u32 value, u32 width, u32 selector, bool sign) {
    const u32 bits = width < 2 ? 8 : width == 2 ? 16 : 32;
    if (bits == 32)
        return value;
    value = (value >> (selector * bits)) & ((1u << bits) - 1);
    if (sign && (value & (1u << (bits - 1))))
        value |= ~((1u << bits) - 1);
    return value;
}

u32 selectMinMax(u32 a, u32 b, bool maximum, bool sign) {
    const bool less =
        sign ? std::bit_cast<i32>(a) < std::bit_cast<i32>(b) : a < b;
    return maximum ? (less ? b : a) : (less ? a : b);
}

void testVideoArithmetic(MTL::Device* device) {
    std::vector<AluState> inputs(5);
    const std::array<u32, 5> av{0x817ffe01u, 0x800100ffu, 0xffffffffu, 0,
                                0x7fffffff};
    const std::array<u32, 5> bv{0xff02807fu, 0xffff8001u, 0x80000000u, 0xffff,
                                0x01020304u};
    for (u32 row = 0; row < inputs.size(); ++row) {
        inputs[row].regs[16] = av[row];
        inputs[row].regs[17] = bv[row];
        inputs[row].regs[18] = 0x80000000u + row;
        inputs[row].preds = row & 1;
        inputs[row].cc = 15;
    }
    struct Case {
        u32 wa, wb, sa, sb;
        bool signa, signb, immediate, guard;
    };
    std::vector<Case> cases;
    for (u32 wa = 0; wa < 4; ++wa)
        for (u32 wb = 0; wb < 4; ++wb)
            for (u32 signs = 0; signs < 4; ++signs) {
                const u32 sa = wa < 2    ? wa * 2 + (wb & 1)
                               : wa == 2 ? wb & 1
                                         : 0;
                const u32 sb = wb < 2    ? wb * 2 + (wa & 1)
                               : wb == 2 ? wa & 1
                                         : 0;
                cases.push_back({wa, wb, sa, sb, (signs & 1) != 0,
                                 (signs & 2) != 0, false, false});
            }
    for (u32 wa = 0; wa < 4; ++wa)
        for (u32 signs = 0; signs < 4; ++signs)
            cases.push_back({wa, 2,
                             wa < 2    ? wa * 2 + 1
                             : wa == 2 ? 1u
                                       : 0u,
                             0, (signs & 1) != 0, (signs & 2) != 0, true,
                             true});
    for (usize first = 0; first < cases.size(); first += 5) {
        const usize count = std::min<usize>(5, cases.size() - first);
        std::vector<u64> program;
        for (usize j = 0; j < count; ++j) {
            const auto& c = cases[first + j];
            const u64 common =
                (16ull << 8) | (u64(c.guard ? 0 : 7) << 16) |
                (u64(c.immediate ? 0xfedc : 17) << 20) |
                (c.immediate ? 0 : ((u64(c.wb) << 29) | (u64(c.sb) << 28))) |
                (u64(c.wa) << 37) | (u64(c.sa) << 36) | (18ull << 39) |
                (u64(c.signa) << 48) | (u64(c.signb) << 49) |
                (u64(!c.immediate) << 50);
            program.push_back(0x5f00000000000000ull | common | (j * 3));
            const u64 minmax_common =
                (common & ~(1ull << 48)) | (u64(c.signb) << 48);
            program.push_back(0x3a00000000000000ull | minmax_common |
                              (5ull << 51) | (1ull << 54) | (j * 3 + 1));
            program.push_back(0x3b00000000000000ull | minmax_common |
                              (6ull << 51) | (j * 3 + 2));
        }
        const auto output =
            runAlu(device, program, inputs, {}, 2, "Video arithmetic");
        for (usize row = 0; row < inputs.size(); ++row)
            for (usize j = 0; j < count; ++j) {
                const auto& c = cases[first + j];
                const auto a = extendVideo(av[row], c.wa, c.sa, c.signa);
                const auto b = extendVideo(c.immediate ? 0xfedc : bv[row],
                                           c.immediate ? 2 : c.wb,
                                           c.immediate ? 0 : c.sb, c.signb);
                const auto minmax_a = extendVideo(av[row], c.wa, c.sa, c.signb);
                const auto third = inputs[row].regs[18];
                const bool enabled = !c.guard || (inputs[row].preds & 1);
                const std::array<u32, 3> expected{
                    enabled ? a * b + third : 0,
                    enabled ? selectMinMax(
                                  selectMinMax(minmax_a, b, false, c.signb),
                                  third, false, true)
                            : 0,
                    enabled
                        ? selectMinMax(selectMinMax(minmax_a, b, true, c.signb),
                                       third, true, false)
                        : 0};
                for (u32 k = 0; k < 3; ++k) {
                    if (output[row].regs[j * 3 + k] != expected[k])
                        std::cerr << "case=" << first + j << " row=" << row
                                  << " op=" << k
                                  << " got=" << output[row].regs[j * 3 + k]
                                  << " expected=" << expected[k] << '\n';
                    expect(
                        output[row].regs[j * 3 + k] == expected[k],
                        "VMAD/VMNMX widths, selectors, signs and predication");
                }
                expect(output[row].cc == 15,
                       "Video base forms preserve condition flags");
            }
    }
    std::cout << "PASS: VMAD/VMNMX signed/unsigned bytes, halfwords, words, "
                 "immediates, predication\n";
}

void testVideoSecondaryModes(MTL::Device* device) {
    std::vector<AluState> inputs(4);
    const std::array<u32, 4> a{0x80000000u, 0xffffffffu, 0x12345678u,
                               0x7fffffffu};
    const std::array<u32, 4> b{0xffffffffu, 0x80000000u, 0x98765432u, 1};
    for (u32 row = 0; row < inputs.size(); ++row) {
        inputs[row].regs[16] = a[row];
        inputs[row].regs[17] = b[row];
        inputs[row].regs[18] = 0xabcdef01u;
    }
    const u64 common = ALWAYS | (16ull << 8) | (17ull << 20) | (3ull << 29) |
                       (3ull << 37) | (18ull << 39) | (1ull << 50);
    std::vector<u64> modes;
    for (u32 mode = 0; mode < 4; ++mode)
        modes.push_back(0x5f00000000000000ull | common | (u64(mode) << 53) |
                        mode);
    const auto output =
        runAlu(device, modes, inputs, {}, 2, "VMAD negate/plus-one");
    for (usize row = 0; row < inputs.size(); ++row) {
        const u32 product = a[row] * b[row];
        const u32 third = inputs[row].regs[18];
        const std::array<u32, 4> expected{product + third, product - third,
                                          third - product, product + third + 1};
        for (u32 mode = 0; mode < 4; ++mode)
            expect(output[row].regs[mode] == expected[mode],
                   "VMAD negate product, negate addend, plus one use wrapping "
                   "arithmetic");
    }
    for (u32 group = 0; group < 16; ++group) {
        const bool signed_source = (group & 1) != 0;
        const bool signed_destination = (group & 2) != 0;
        const bool saturate = (group & 4) != 0;
        const bool maximum = (group & 8) != 0;
        std::vector<u64> program;
        for (u32 op = 0; op < 8; ++op)
            program.push_back(0x3a00000000000000ull | common | op |
                              (u64(signed_source) << 48) |
                              (u64(signed_source) << 49) | (u64(op) << 51) |
                              (u64(signed_destination) << 54) |
                              (u64(saturate) << 55) | (u64(maximum) << 56));
        const auto results =
            runAlu(device, program, inputs, {}, 2,
                   "VMNMX secondary operations and saturation");
        for (usize row = 0; row < inputs.size(); ++row) {
            u32 first = selectMinMax(a[row], b[row], maximum, signed_source);
            if (saturate && signed_source != signed_destination) {
                if (signed_destination)
                    first = std::min(first, 0x7fffffffu);
                else if (std::bit_cast<i32>(first) < 0)
                    first = 0;
            }
            const u32 third = inputs[row].regs[18];
            const std::array<u32, 8> expected{
                (third & 0x0000ffffu) | (first << 16),
                (third & 0xffff0000u) | (first & 0xffffu),
                (third & 0xffffff00u) | (first & 0xffu),
                (third & 0xff00ffffu) | ((first & 0xffu) << 16),
                first + third,
                selectMinMax(first, third, false, signed_destination),
                selectMinMax(first, third, true, signed_destination),
                first};
            for (u32 op = 0; op < 8; ++op)
                expect(results[row].regs[op] == expected[op],
                       "VMNMX merge/accumulate/min/max/raw and pre-secondary "
                       "saturation");
        }
    }
    std::cout << "PASS: VMAD negate/plus-one; VMNMX all secondary operations "
                 "and saturation domains\n";
}

bool compareVideo(u32 op, i32 a, i32 b) {
    switch (op) {
    case 0:
        return false;
    case 1:
        return a < b;
    case 2:
        return a == b;
    case 3:
        return a <= b;
    case 4:
        return a > b;
    case 5:
        return a != b;
    case 6:
        return a >= b;
    case 7:
        return true;
    }
    return false;
}

void testVideoPredicates(MTL::Device* device) {
    std::vector<AluState> inputs;
    for (const u32 raw : {0x0000007fu, 0x00000080u, 0x000000ffu})
        for (u32 pred = 0; pred < 2; ++pred) {
            AluState state{};
            state.regs[16] = raw;
            state.regs[17] = 0x7f;
            state.preds = pred;
            inputs.push_back(state);
        }
    for (u32 compare = 0; compare < 8; ++compare)
        for (u32 bop = 0; bop < 3; ++bop) {
            // P0 is both an input and the first destination; snapshotting its
            // old value is required before the second destination is written.
            const u64 instruction =
                0x50f0000000000000ull | ALWAYS | 1ull | (16ull << 8) |
                (17ull << 20) | (u64(compare & 3) << 43) | (u64(bop) << 45) |
                (u64(compare >> 2) << 47) | (1ull << 48) | (1ull << 49) |
                (1ull << 50);
            const auto output = runAlu(device, {instruction}, inputs, {}, 2,
                                       "VSETP compare and boolean modes");
            for (usize row = 0; row < inputs.size(); ++row) {
                const auto cmp =
                    compareVideo(compare,
                                 std::bit_cast<i32>(extendVideo(
                                     inputs[row].regs[16], 0, 0, true)),
                                 127);
                const bool old = inputs[row].preds != 0;
                const auto combine = [bop, old](bool v) {
                    return bop == 0 ? v && old : bop == 1 ? v || old : v != old;
                };
                const u32 expected =
                    u32(combine(cmp)) | (u32(combine(!cmp)) << 1);
                expect(output[row].preds == expected,
                       "VSETP all comparisons, AND/OR/XOR, aliased input "
                       "predicate");
            }
        }
    // Immediate bits28..29 are part of the immediate, never a lane selector.
    const u64 immediate = 0x50f0000000000000ull | ALWAYS | 1ull | (16ull << 8) |
                          (0xff80ull << 20) | (2ull << 43) | (7ull << 39) |
                          (1ull << 48) | (1ull << 49);
    const auto immediate_output =
        runAlu(device, {immediate}, inputs, {}, 2, "VSETP signed immediate");
    for (usize row = 0; row < inputs.size(); ++row)
        expect(immediate_output[row].preds ==
                   (inputs[row].regs[16] == 0x80 ? 1u : 2u),
               "VSETP sign extends the full 16-bit immediate and ignores its "
               "selector bits");
    std::cout << "PASS: VSETP comparisons, predicate combining/aliasing, "
                 "signed immediate\n";
}
} // namespace

int main() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
    try {
        expect(device != nullptr, "Metal device exists");
        testVideoArithmetic(device);
        testVideoSecondaryModes(device);
        testVideoPredicates(device);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

#include "core/hw/tegra_x1/gpu/renderer/metal/clear_color_pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/depth_stencil_state_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/compute.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/sampler.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer::metal;
class ConstantEmitter
    : public hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::LangEmitter {
  public:
    using LangEmitter::getConstantStr;
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Stage I/O, special registers and instruction families
// (shader_coverage_tests.cpp)
void runShaderCoverageTests(MTL::Device* device);
// Runs the blocks of main in order of their returned labels
std::string runProgramSource(const hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir::Module& module);
// Regression for predicate encodings that previously hid valid texture handles.
void testBindlessPredicateTracking() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    for (u64 predicate : {0x5b64038000470307ull, 0x5bb4838001071d1full,
                          0x4bbd838c2c77ff07ull}) {
        const reg_t reg = static_cast<u8>(predicate & 255u);
        const std::array<u64, 4> words{0,
                                       0x4c98000000070000ull |
                                           static_cast<u64>(reg) |
                                           (2ull << 34) | (9ull << 20),
                                       predicate, 0};
        std::vector<u8> bytes(sizeof(words));
        std::memcpy(bytes.data(), words.data(), sizeof(words));
        ztd::io::MemoryStream stream(bytes);
        stream.seekTo(sizeof(words));
        DecompilerContext context{};
        ir::Module module;
        ir::Builder builder(module);
        decoder::DecoderContext decoder_context{context, &stream, builder};
        const auto handle =
            decoder::trackBindlessResource(decoder_context, reg);
        check(handle && *handle == makeTrackedTextureIndex(
                                       2, 9, TRACKED_TEXTURE_NO_SAMPLER),
              "Predicate-only instructions do not overwrite texture handles");
        check(stream.getSeek() == sizeof(words),
              "Texture tracking restores stream position");
    }
    for (u32 component = 0; component < 4; ++component) {
        const std::array<u64, 4> words{0,
                                       0xef96000000070004ull | (255ull << 8) |
                                           (2ull << 36) | (16ull << 20),
                                       0x50b0000000070000ull, 0};
        std::vector<u8> bytes(sizeof(words));
        std::memcpy(bytes.data(), words.data(), sizeof(words));
        ztd::io::MemoryStream stream(bytes);
        stream.seekTo(sizeof(words));
        DecompilerContext context{};
        ir::Module module;
        ir::Builder builder(module);
        decoder::DecoderContext dc{context, &stream, builder};
        const auto handle =
            decoder::trackBindlessResource(dc, reg_t(4u + component));
        check(handle && *handle ==
                            makeTrackedTextureIndex(2, 4 + component,
                                                    TRACKED_TEXTURE_NO_SAMPLER),
              "All four LDC.B128 words can provide texture handles");
    }
}

// Handle registers can be defined before a branch that skips code ending in
// EXIT, or be shader constants.
void testBindlessControlFlowTracking() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    const auto track = [](const std::vector<u64>& words, reg_t reg) {
        std::vector<u8> bytes(words.size() * 8);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        ztd::io::MemoryStream stream(bytes);
        stream.seekTo(bytes.size());
        DecompilerContext context{};
        ir::Module module;
        ir::Builder builder(module);
        decoder::DecoderContext dc{context, &stream, builder};
        return decoder::trackBindlessResource(dc, reg);
    };
    constexpr u64 NOP = 0x50b0000000070f00ull;
    const std::vector<u64> skipped_exit{
        0,
        0x4c98000000070000ull | 10 | (2ull << 34) | (4ull << 20), // MOV c
        0xe24000000008000full | (24ull << 20), // @!P0 BRA 6
        0x5c9807800ff7000aull,                // MOV R10, RZ
        0,
        0xe30000000007000full, // EXIT
        NOP,
        NOP};
    check(track(skipped_exit, 10) ==
              makeTrackedTextureIndex(2, 4, TRACKED_TEXTURE_NO_SAMPLER),
          "Handle defined before a branch over an exiting block");
    const std::vector<u64> constant{0, 0x5c9807800ff70000ull, NOP, NOP};
    check(track(constant, 0) == makeConstantTextureIndex(0),
          "Constant zero handle");
    const std::vector<u64> constant32{0, 0x0100000000000f00ull | (0x123ull << 20),
                                      NOP, NOP};
    check(track(constant32, 0) == makeConstantTextureIndex(0x123),
          "MOV32I handle");
    std::cout << "PASS: bindless handles across exiting branches and constant "
                 "handles\n";
}

// Execute the actual decoded instructions on Metal and compare with 64-bit CPU
// reference arithmetic, including shift boundaries and predicated writes.
void testIntegerConditions(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    DecompilerContext context{}; context.type=ShaderType::Compute;
    ir::Module module; ir::Builder builder(module);
    decoder::DecoderContext ctx{context,nullptr,builder};
    for (u32 variant=0;variant<4;variant++) for (u32 mode=0;mode<3;mode++) {
        const u64 base=255ull | (1ull<<8) | (7ull<<16);
        if (variant==0) decoder::emitIaddR(ctx,std::bit_cast<decoder::InstIaddR>(base | (2ull<<20) | (1ull<<47) | (u64(mode)<<48)));
        if (variant==1) decoder::emitIaddC(ctx,std::bit_cast<decoder::InstIaddC>(base | (2ull<<20) | (2ull<<34) | (1ull<<47) | (u64(mode)<<48)));
        if (variant==2) decoder::emitIaddI(ctx,std::bit_cast<decoder::InstIaddI>(base | (1ull<<20) | (1ull<<47) | (u64(mode)<<48)));
        if (variant==3) decoder::emitIadd32I(ctx,std::bit_cast<decoder::InstIadd32I>(base | (1ull<<20) | (1ull<<52) | (u64(mode)<<55)));
        for (u32 i=0;i<4;i++) builder.opCopy(ir::Value::createRegister(static_cast<reg_t>(16+(variant*3+mode)*6+i)),
            builder.opSelect(ir::Value::createConditionFlag(i),ir::Value::createConstantU(1),ir::Value::createConstantU(0)));
        decoder::emitCsetp(ctx, 1ull | (13ull<<8) | (7ull<<16) | (7ull<<39));
        for(u32 i=0;i<2;i++) builder.opCopy(ir::Value::createRegister(static_cast<reg_t>(16+(variant*3+mode)*6+4+i)),
            builder.opSelect(ir::Value::createPredicate(static_cast<pred_t>(i)),ir::Value::createConstantU(1),ir::Value::createConstantU(0)));
    }
    builder.opExit(); analyzer::MemoryAnalyzer memory; memory.analyze(module);
    GuestShaderState guest{}; ResourceMapping mapping; std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context,memory,guest,generated,mapping); emitter.emit(module);
    std::string source(generated.begin(),generated.end()); source += runProgramSource(module);
    source += "\nkernel void check_cc(device uint* out [[buffer(0)]], constant Reg* in [[buffer(1)]]) { State state; state.c2=in; state.r[1]=in[1]; state.r[2]=in[2]; run_program(state); for(uint i=0;i<72;i++) out[i]=state.r[16+i]._u32; }";
    NS::Error* error=nullptr; NS_STACK_SCOPED auto library=device->newLibrary(toNsString(source),nullptr,&error);
    if (!library && error) std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr,"IADD CC/CSETP compiles");
    NS_STACK_SCOPED auto function=library->newFunction(toNsString("check_cc"));
    NS_STACK_SCOPED auto pipeline=device->newComputePipelineState(function,&error);
    check(pipeline != nullptr,"IADD CC/CSETP pipeline compiles");
    NS_STACK_SCOPED auto queue=device->newCommandQueue();
    NS_STACK_SCOPED auto output=device->newBuffer(72*4,MTL::ResourceStorageModeShared);
    for(u32 a:{0u,1u,0x7fffffffu,0x80000000u,0xffffffffu}) for(u32 b:{0u,1u,0x7fffffffu,0x80000000u,0xffffffffu}) {
        const std::array<u32,3> input{0,a,b}; auto* command=queue->commandBuffer(); auto* encoder=command->computeCommandEncoder();
        encoder->setComputePipelineState(pipeline); encoder->setBuffer(output,0,0); encoder->setBytes(input.data(),sizeof(input),1);
        encoder->dispatchThreads(MTL::Size(1,1,1),MTL::Size(1,1,1)); encoder->endEncoding(); command->commit(); command->waitUntilCompleted();
        check(command->status()==MTL::CommandBufferStatusCompleted,"IADD CC/CSETP executes");
        const auto* result=static_cast<const u32*>(output->contents());
        for(u32 variant=0;variant<4;variant++) for(u32 mode=0;mode<3;mode++) {
            const u32 aa=mode==2 ? 0u-a : a;
            u32 bb=variant<2 ? b : 1;
            if(mode==1) bb=0u-bb;
            const u64 sum=u64(aa)+bb; const u32 value=static_cast<u32>(sum);
            const std::array<u32,6> expected{value==0, value>>31, u32(sum>>32), ((~(aa^bb)&(aa^value))>>31), value!=0, value==0};
            for(u32 i=0;i<6;i++) check(result[(variant*3+mode)*6+i]==expected[i],"IADD flags survive discarded RZ result and drive CSETP NEU");
        }
    }
    std::cout << "PASS: GPU IADD condition flags and CSETP NEU with RZ destination and overflow boundaries" << std::endl;
}

void testRegisterPredicates(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    DecompilerContext context{}; context.type = ShaderType::Compute;
    ir::Module module; ir::Builder builder(module);
    decoder::DecoderContext decoder_context{context, nullptr, builder};
    for (u32 variant=0;variant<3;variant++) for (u32 byte=0;byte<4;byte++) {
        for (u32 i=0;i<7;i++) builder.opCopy(ir::Value::createPredicate(static_cast<pred_t>(i)), ir::Value::createConstantB(i%2!=0));
        const u64 inst = (1ull<<8) | (7ull<<16) | ((variant==2 ? 6ull : 2ull)<<20) |
                         (variant==1 ? 2ull<<34 : 0) | (u64(byte)<<41);
        decoder::emitR2p(decoder_context, inst, variant);
        for (u32 i=0;i<7;i++) builder.opCopy(ir::Value::createRegister(static_cast<reg_t>(16+(variant*4+byte)*7+i)),
            builder.opSelect(ir::Value::createPredicate(static_cast<pred_t>(i)), ir::Value::createConstantU(1), ir::Value::createConstantU(0)));
    }
    builder.opExit(); analyzer::MemoryAnalyzer memory; memory.analyze(module);
    GuestShaderState guest{}; ResourceMapping mapping; std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context,memory,guest,generated,mapping); emitter.emit(module);
    std::string source(generated.begin(),generated.end()); source += runProgramSource(module);
    source += "\nkernel void check_pred(device uint* out [[buffer(0)]], constant Reg* in [[buffer(1)]]) { State state; state.c2=in; state.r[1]=in[1]; state.r[2]=in[2]; run_program(state); for(uint i=0;i<84;i++) out[i]=state.r[16+i]._u32; }";
    NS::Error* error=nullptr;
    NS_STACK_SCOPED auto library=device->newLibrary(toNsString(source),nullptr,&error);
    if (!library && error) std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr,"R2P variants compile");
    NS_STACK_SCOPED auto function=library->newFunction(toNsString("check_pred"));
    NS_STACK_SCOPED auto pipeline=device->newComputePipelineState(function,&error);
    check(pipeline != nullptr,"R2P pipeline compiles");
    NS_STACK_SCOPED auto queue=device->newCommandQueue();
    NS_STACK_SCOPED auto output=device->newBuffer(84*4,MTL::ResourceStorageModeShared);
    for (u32 value : {0u,0xffffffffu,0xdeadbeefu,0x01020408u}) for (u32 mask : {0u,6u,7u,0x55u,127u}) {
        const std::array<u32,3> input{0,value,mask};
        auto* command=queue->commandBuffer(); auto* encoder=command->computeCommandEncoder();
        encoder->setComputePipelineState(pipeline); encoder->setBuffer(output,0,0); encoder->setBytes(input.data(),sizeof(input),1);
        encoder->dispatchThreads(MTL::Size(1,1,1),MTL::Size(1,1,1)); encoder->endEncoding(); command->commit(); command->waitUntilCompleted();
        check(command->status()==MTL::CommandBufferStatusCompleted,"R2P executes");
        const auto* result=static_cast<const u32*>(output->contents());
        for(u32 variant=0;variant<3;variant++) for(u32 byte=0;byte<4;byte++) for(u32 i=0;i<7;i++) {
            const u32 actual_mask=variant==2 ? 6 : mask;
            const u32 expected=(actual_mask&(1u<<i)) ? ((value>>(byte*8+i))&1u) : i%2;
            check(result[(variant*4+byte)*7+i]==expected,"R2P selects byte, updates masked predicates and preserves other predicates");
        }
    }
    std::cout << "PASS: GPU R2P mask preservation, byte selectors and register/cbuf/immediate variants" << std::endl;
}

void testLocalMemory(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    DecompilerContext context{}; context.type = ShaderType::Compute;
    context.local_memory_size = 0x40;
    ir::Module module; ir::Builder builder(module);
    decoder::DecoderContext decoder_context{context, nullptr, builder};
    const auto make = [](u32 data, u32 offset, u32 size) {
        return u64(data) | (255ull << 8) | (7ull << 16) | (u64(offset) << 20) | (u64(size) << 48);
    };
    decoder::emitStl(decoder_context, make(1, 0x10, 4));
    decoder::emitStl(decoder_context, make(1, 0x20, 5));
    decoder::emitLdl(decoder_context, make(16, 0x10, 4));
    decoder::emitLdl(decoder_context, make(17, 0x20, 5));
    decoder::emitLdl(decoder_context, make(19, 0x11, 0));
    decoder::emitLdl(decoder_context, make(20, 0x13, 1));
    decoder::emitLdl(decoder_context, make(21, 0x12, 2));
    decoder::emitLdl(decoder_context, make(22, 0x12, 3));
    builder.opExit(); analyzer::MemoryAnalyzer memory; memory.analyze(module);
    GuestShaderState guest{}; ResourceMapping mapping; std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context,memory,guest,generated,mapping); emitter.emit(module);
    std::string source(generated.begin(),generated.end()); source += runProgramSource(module);
    source += "\nkernel void check_local(device uint* out [[buffer(0)]], constant Reg* in [[buffer(1)]]) { State state; state.r[1]=in[1]; state.r[2]=in[2]; run_program(state); for(uint i=0;i<7;i++) out[i]=state.r[16+i]._u32; out[1]=state.r[17]._u32; out[2]=state.r[18]._u32; }";
    NS::Error* error=nullptr;
    NS_STACK_SCOPED auto library=device->newLibrary(toNsString(source),nullptr,&error);
    if (!library && error) std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr,"LDL/STL compile");
    NS_STACK_SCOPED auto function=library->newFunction(toNsString("check_local"));
    NS_STACK_SCOPED auto pipeline=device->newComputePipelineState(function,&error);
    check(pipeline != nullptr,"LDL/STL pipeline compiles");
    NS_STACK_SCOPED auto queue=device->newCommandQueue();
    NS_STACK_SCOPED auto output=device->newBuffer(7*4,MTL::ResourceStorageModeShared);
    for (u32 value : {0u,0x12345678u,0x80ff7f01u,0xdeadbeefu}) {
        const std::array<u32,3> input{0,value,~value};
        auto* command=queue->commandBuffer(); auto* encoder=command->computeCommandEncoder();
        encoder->setComputePipelineState(pipeline); encoder->setBuffer(output,0,0); encoder->setBytes(input.data(),sizeof(input),1);
        encoder->dispatchThreads(MTL::Size(1,1,1),MTL::Size(1,1,1)); encoder->endEncoding(); command->commit(); command->waitUntilCompleted();
        check(command->status()==MTL::CommandBufferStatusCompleted,"LDL/STL executes");
        const auto* r=static_cast<const u32*>(output->contents());
        check(r[0]==value,"LDL.B32 returns the STL.B32 value");
        check(r[1]==value && r[2]==~value,"LDL.B64 returns both STL.B64 words");
        check(r[3]==((value>>8)&0xff),"LDL.U8 extracts a byte");
        check(r[4]==u32(i32(i8(value>>24))),"LDL.S8 sign extends a byte");
        check(r[5]==((value>>16)&0xffff),"LDL.U16 extracts a half word");
        check(r[6]==u32(i32(i16(value>>16))),"LDL.S16 sign extends a half word");
    }
    std::cout << "PASS: GPU local memory LDL/STL 8/16/32/64-bit accesses" << std::endl;
}

void testSharedAtomics(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    std::vector<u64> words;
    const auto append = [&](u64 x) { if (words.size() % 4 == 0) words.push_back(0); words.push_back(x); };
    for (u32 i = 0; i < 13; ++i) {
        const u32 op = i < 9 ? i : i == 9 ? 1 : i == 10 ? 2 : 0;
        const u32 size = (i == 9 || i == 10) ? 1 : 0;
        const u32 pred = i >= 11 ? 0 : 7;
        append(0xec00000000000000ull | (16ull + i) | (255ull << 8) |
               (u64(pred) << 16) | (i == 12 ? (1ull << 19) : 0) |
               (u64(size ? 2 : 1) << 20) | (u64(size) << 28) |
               (u64(i) << 30) | (u64(op) << 52));
    }
    // Exercise a signed relative offset and discarded return value.
    append(0xec000000000000ffull | (3ull << 8) | (7ull << 16) |
           (1ull << 20) | (0x3fffffull << 30));
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8); std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{}; context.type = ShaderType::Compute;
    ir::Module module; ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder}); decoder.decode();
    analyzer::MemoryAnalyzer memory; memory.analyze(module);
    GuestShaderState guest{}; guest.shared_memory_size = 64;
    ResourceMapping mapping; std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated, mapping); emitter.emit(module);
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += R"(
        kernel void check_atomic(device uint* out [[buffer(0)]], uint tid [[thread_index_in_threadgroup]]) {
            threadgroup Reg words[16];
            if (tid == 0) {
                for (uint i=0;i<16;i++) words[i]._u32=0;
                words[1]._u32=999; words[5]._u32=0xffffffffu;
                words[9]._u32=0x7fffffffu; words[10]._u32=0x80000000u;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            State state; state.smem=words; state.r[1]._u32=1;
            state.r[2]._u32=uint(int(tid)-32); state.r[3]._u32=56;
            state.p[0]=(tid&1u)!=0;
            run_program(state);
            out[16+tid]=state.r[16]._u32;
            threadgroup_barrier(mem_flags::mem_threadgroup);
            if (tid==0) for (uint i=0;i<16;i++) out[i]=words[i]._u32;
        }
    )";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library = device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error) std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "Decoded shared atomics compile");
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("check_atomic"));
    NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "Shared atomic pipeline compiles");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    NS_STACK_SCOPED auto output = device->newBuffer(80 * 4, MTL::ResourceStorageModeShared);
    auto* command = queue->commandBuffer(); auto* encoder = command->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline); encoder->setBuffer(output, 0, 0);
    encoder->dispatchThreads(MTL::Size(64,1,1), MTL::Size(64,1,1));
    encoder->endEncoding(); command->commit(); command->waitUntilCompleted();
    check(command->status() == MTL::CommandBufferStatusCompleted, "Contended shared atomics execute");
    const auto* result = static_cast<const u32*>(output->contents());
    const std::array<u32, 16> expected{64,1,1,0,0,1,1,0,1,0xffffffe0u,31,32,32,64,0,0};
    for (u32 i=0;i<16;i++) {
        if (result[i] != expected[i]) std::cerr << fmt::format("atomic word {} got {:x} expected {:x}\n", i, result[i], expected[i]);
        check(result[i] == expected[i], "Atomic operators preserve concurrent writes, signedness, predicates and offsets");
    }
    std::vector<u32> previous(result + 16, result + 80); std::sort(previous.begin(), previous.end());
    for (u32 i=0;i<64;i++) check(previous[i] == i, "Each contended atomic add returns its unique previous value");
    std::cout << "PASS: GPU shared atomics under 64-thread contention, signed min/max, wrap, predicates and relative offset" << std::endl;
}

void testIntegerShifts(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    struct Case {
        bool right, wrap, sign, brev;
        u32 variant;
    };
    std::vector<Case> cases;
    std::vector<u64> words;
    const auto append = [&](u64 x) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(x);
    };
    for (bool right : {false, true})
        for (bool wrap : {false, true})
            for (u32 mode = 0; mode < (right ? 4u : 1u); ++mode)
                for (u32 variant = 0; variant < 3; ++variant) {
                    const std::array<u64, 3> ops =
                        right ? std::array<u64, 3>{0x5c28000000000000ull,
                                                   0x4c28000000000000ull,
                                                   0x3828000000000000ull}
                              : std::array<u64, 3>{0x5c48000000000000ull,
                                                   0x4c48000000000000ull,
                                                   0x3848000000000000ull};
                    const bool sign = mode & 1, brev = mode & 2;
                    append(
                        ops[variant] | (16 + cases.size()) | (1ull << 8) |
                        (7ull << 16) | ((variant == 2 ? 32ull : 2ull) << 20) |
                        (variant == 1 ? (2ull << 34) : 0) | (u64(wrap) << 39) |
                        (u64(brev) << 40) | (u64(sign) << 48));
                    cases.push_back({right, wrap, sign, brev, variant});
                }
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    ResourceMapping mapping;
    std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += fmt::format(
        "\nkernel void check_shift(device uint* out [[buffer(0)]], constant "
        "Reg* in [[buffer(1)]]) {{ State state; state.c2=in; state.r[1]=in[1]; "
        "state.r[2]=in[2]; run_program(state); for(uint i=0;i<{};i++) "
        "out[i]=state.r[16+i]._u32; }}",
        cases.size());
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "SHL/SHR shaders compile");
    NS_STACK_SCOPED auto function =
        library->newFunction(toNsString("check_shift"));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "SHL/SHR pipeline compiles");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    NS_STACK_SCOPED auto output =
        device->newBuffer(cases.size() * 4, MTL::ResourceStorageModeShared);
    for (u32 value : {0u, 1u, 0x12345678u, 0x80000001u, 0xffffffffu})
        for (u32 shift : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 0xffffffffu}) {
            const std::array<u32, 3> input{0, value, shift};
            auto* command = queue->commandBuffer();
            auto* encoder = command->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->setBytes(input.data(), sizeof(input), 1);
            encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
            encoder->endEncoding();
            command->commit();
            command->waitUntilCompleted();
            check(command->status() == MTL::CommandBufferStatusCompleted,
                  "SHL/SHR executes");
            const auto* result = static_cast<const u32*>(output->contents());
            for (usize i = 0; i < cases.size(); ++i) {
                const auto& c = cases[i];
                u32 base = value;
                if (c.brev) {
                    base = 0;
                    for (u32 bit = 0; bit < 32; ++bit)
                        base |= ((value >> bit) & 1u) << (31 - bit);
                }
                u32 n = c.variant == 2 ? 32 : shift;
                if (c.wrap)
                    n &= 31;
                const u32 expected =
                    n >= 32
                        ? (c.right && c.sign && (base & 0x80000000u)
                               ? 0xffffffffu
                               : 0u)
                        : (c.right ? (c.sign ? u32(i32(base) >> n) : base >> n)
                                   : base << n);
                if (result[i] != expected)
                    std::cerr << fmt::format("shift case {} value {:x} n {} "
                                             "got {:x} expected {:x}\n",
                                             i, value, n, result[i], expected);
                check(result[i] == expected,
                      "Signed, reversed, wrapped and clamped shifts preserve "
                      "guest semantics");
            }
        }
    std::cout << "PASS: GPU SHL/SHR signed, reversed, wrapped/clamped and "
                 "operand variants"
              << std::endl;
}

void testBitOperations(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    struct Case {
        u64 opcode;
        u32 mode;
        bool wrap;
        bool right;
        bool immediate;
        u32 shift;
    };
    std::vector<Case> cases;
    std::vector<u64> words;
    const auto append = [&](u64 x) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(x);
    };
    for (u32 mode : {0u, 2u, 3u}) {
        for (bool wrap : {false, true}) {
            for (bool right : {false, true}) {
                for (int shift : {-1, 0, 1, 31, 32, 33, 63}) {
                    const bool immediate = shift >= 0;
                    const u64 opcode = immediate
                                           ? (right ? 0x38f8000000000000ull
                                                    : 0x36f8000000000000ull)
                                           : (right ? 0x5cf8000000000000ull
                                                    : 0x5bf8000000000000ull);
                    append(opcode | (16ull + cases.size()) | (1ull << 8) |
                           (7ull << 16) |
                           (static_cast<u64>(immediate ? shift : 2) << 20) |
                           (static_cast<u64>(mode) << 37) | (3ull << 39) |
                           (static_cast<u64>(wrap) << 50));
                    cases.push_back({opcode, mode, wrap, right, immediate,
                                     static_cast<u32>(shift)});
                }
            }
        }
    }
    const u32 bfi_start = static_cast<u32>(cases.size());
    const std::array<u64, 4> bfi{0x5bf0000000000000ull, 0x4bf0000000000000ull,
                                 0x53f0000000000000ull, 0x36f0000000000000ull};
    for (u32 variant = 0; variant < 4; ++variant) {
        u64 x = bfi[variant] | (16u + bfi_start + variant) | (1ull << 8) |
                (7ull << 16);
        if (variant == 0)
            x |= (2ull << 20) | (3ull << 39);
        if (variant == 1)
            x |= (2ull << 34) | (2ull << 20) | (3ull << 39);
        if (variant == 2)
            x |= (2ull << 34) | (3ull << 20) | (2ull << 39);
        if (variant == 3)
            x |= (0x2000ull << 20) | (3ull << 39); // offset 0, full 32 bits
        append(x);
    }
    const u32 pred_index = bfi_start + 4;
    append(bfi[0] | (16u + pred_index) | (1ull << 8) | (2ull << 20) |
           (3ull << 39)); // P0
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    ResourceMapping mapping;
    std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    const u32 count = pred_index + 1;
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += fmt::format(
        "\nkernel void check_bits(device uint* out [[buffer(0)]], constant "
        "Reg* in [[buffer(1)]], uint tid [[thread_position_in_grid]]) {{ State "
        "state; state.c2=in; state.r[1]=in[1]; state.r[2]=in[2]; "
        "state.r[3]=in[3]; state.p[0]=tid!=0; for(uint i=16;i<{};i++) "
        "state.r[i]._u32=0x12345678u; run_program(state); for(uint "
        "i=0;i<{};i++) out[tid*{}+i]=state.r[16+i]._u32; }}",
        16 + count, count, count);
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "BFI/SHF guest instructions compile");
    NS_STACK_SCOPED auto function =
        library->newFunction(toNsString("check_bits"));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "Bit operation pipeline exists");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    NS_STACK_SCOPED auto output =
        device->newBuffer(2 * count * 4, MTL::ResourceStorageModeShared);
    for (u32 control : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 255u, 0x100u, 0x2000u,
                        0x201fu, 0xff1fu, 0x0820u, 0x0804u, 0xffffffffu}) {
        for (u32 hi : {0x12345678u, 0x87654321u}) {
            constexpr u32 lo = 0xa5a5c3c3u;
            const std::array<u32, 4> in{0, lo, control, hi};
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->setBytes(in.data(), sizeof(in), 1);
            encoder->dispatchThreads(MTL::Size(2, 1, 1), MTL::Size(2, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted,
                  "Bit operations execute");
            const auto* result = static_cast<const u32*>(output->contents());
            for (u32 lane = 0; lane < 2; ++lane) {
                for (u32 index = 0; index < cases.size(); ++index) {
                    const auto& c = cases[index];
                    u32 shift = c.immediate ? c.shift : control;
                    const u32 limit = c.mode == 0 ? 32 : 64;
                    shift =
                        c.wrap ? shift & (limit - 1) : std::min(shift, limit);
                    const u64 packed = (static_cast<u64>(hi) << 32) | lo;
                    u32 expected;
                    if (c.right && c.mode == 3) {
                        const auto signed_packed = std::bit_cast<i64>(packed);
                        expected =
                            shift == 64
                                ? static_cast<u32>(signed_packed < 0 ? -1 : 0)
                                : static_cast<u32>(signed_packed >> shift);
                    } else if (c.right)
                        expected =
                            shift == 64 ? 0 : static_cast<u32>(packed >> shift);
                    else
                        expected =
                            shift == 64
                                ? 0
                                : static_cast<u32>((packed << shift) >> 32);
                    if (result[lane * count + index] != expected)
                        throw std::runtime_error(fmt::format(
                            "SHF case {} shift {}: {:x} != {:x}", index,
                            control, result[lane * count + index], expected));
                }
                const u32 offset = control & 255;
                const u32 n = offset < 32
                                  ? std::min((control >> 8) & 255, 32 - offset)
                                  : 0;
                const u32 mask = n == 32 ? 0xffffffffu
                                         : static_cast<u32>(((1ull << n) - 1)
                                                            << (offset & 31));
                const u32 expected =
                    (hi & ~mask) | ((lo << (offset & 31)) & mask);
                for (u32 variant = 0; variant < 4; ++variant)
                    check(result[lane * count + bfi_start + variant] ==
                              (variant == 3 ? lo : expected),
                          "BFI inserts masked bits and preserves base");
                check(result[lane * count + pred_index] ==
                          (lane ? expected : 0x12345678u),
                      "BFI obeys predicate");
            }
        }
    }
    std::cout << "PASS: GPU funnel shifts, bitfield insertion and "
                 "predicate-safe texture tracking"
              << std::endl;
}

void testIndirectBranches(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    for (bool table : {false, true}) {
        for (bool changed : {false, true}) {
            std::array<u64, 19> words{};
            words[1] = 0x3820000000000000ull | 1 | (1ull << 8) | (7ull << 16) |
                       (2ull << 20) | (7ull << 39);
            words[2] = 0x3848000000000000ull | 1 | (1ull << 8) | (7ull << 16) |
                       (static_cast<u64>(table ? 2 : 5) << 20);
            words[3] = table ? 0xef94000000000000ull | 1 | (1ull << 8) |
                                   (7ull << 16) | (2ull << 36)
                             : 0x50b0000000070000ull;
            // P0 false must continue through the ordinary fallthrough block.
            words[5] = 0xe25000000000000full | (1ull << 8) |
                       (static_cast<u64>(table ? 0 : 24) << 20);
            words[6] =
                0x0100000000000000ull | 16 | (7ull << 16) | (777ull << 20);
            words[7] = 0xe30000000007000full;
            for (u32 index = 0; index < 3; ++index) {
                words[9 + index * 4] =
                    0x0100000000000000ull | 16 | (7ull << 16) |
                    (static_cast<u64>(100 * (index + 1)) << 20);
                words[10 + index * 4] = 0xe30000000007000full;
            }
            std::array<u32, 3> offsets{24, 56, 88};
            if (changed)
                std::swap(offsets[0], offsets[2]);
            std::vector<u8> bytes(sizeof(words));
            std::memcpy(bytes.data(), words.data(), sizeof(words));
            ztd::io::MemoryStream stream(bytes);
            DecompilerContext context{};
            context.type = ShaderType::Compute;
            std::map<std::pair<u32, u32>, u32> dependencies;
            context.branch_constants = &dependencies;
            context.read_const_word = [&](u32 slot,
                                          u32 word) -> std::optional<u32> {
                return slot == 2 && word < offsets.size()
                           ? std::optional(offsets[word])
                           : std::nullopt;
            };
            ir::Module module;
            ir::Builder builder(module);
            decoder::Decoder decoder({context, &stream, builder});
            decoder.decode();
            check(module.getFunction("main").getBlocks().size() == 5,
                  "BRX decodes bounded targets and fallthrough");
            check(dependencies.size() == (table ? 3 : 0),
                  "BRX records exact jump-table words for cache invalidation");
            analyzer::MemoryAnalyzer memory;
            memory.analyze(module);
            GuestShaderState guest{};
            ResourceMapping mapping;
            std::vector<u8> generated;
            codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                                   generated, mapping);
            emitter.emit(module);
            std::string source(generated.begin(), generated.end());
            source += "\nkernel void check_branch(device uint* out "
                      "[[buffer(0)]], constant Reg* in [[buffer(1)]], uint tid "
                      "[[thread_position_in_grid]]) { State state; "
                      "state.c2=in; state.r[1]._u32=tid/2; "
                      "state.p[0]=(tid&1u)!=0; auto next=Block_main::label0x0; "
                      "for(uint guard=0;next!=Block_main::None && "
                      "guard<16;guard++) { switch(next) {";
            for (const auto& [label, block] :
                 module.getFunction("main").getBlocks())
                source += fmt::format(
                    "case Block_main::{}: next=func_{}(state); break;", label,
                    label);
            source += "default: next=Block_main::None; break; } } "
                      "out[tid]=state.r[16]._u32; }";
            if (!table)
                source.erase(source.find("state.c2=in;"),
                             std::string("state.c2=in;").size());
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                device->newLibrary(toNsString(source), nullptr, &error);
            if (!library && error)
                std::cerr << error->localizedDescription()->utf8String()
                          << std::endl;
            check(library != nullptr, "Bounded indirect branch compiles");
            NS_STACK_SCOPED auto function =
                library->newFunction(toNsString("check_branch"));
            NS_STACK_SCOPED auto pipeline =
                device->newComputePipelineState(function, &error);
            check(pipeline != nullptr, "Indirect branch pipeline exists");
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            NS_STACK_SCOPED auto output =
                device->newBuffer(8 * 4, MTL::ResourceStorageModeShared);
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->setBytes(offsets.data(), sizeof(offsets), 1);
            encoder->dispatchThreads(MTL::Size(8, 1, 1), MTL::Size(8, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted,
                  "Indirect branch executes");
            const auto* result = static_cast<const u32*>(output->contents());
            for (u32 tid = 0; tid < 8; ++tid) {
                u32 index = std::min(tid / 2, 2u);
                if (table && changed)
                    index = 2 - index;
                check(result[tid] == ((tid & 1u) ? 100 * (index + 1) : 777),
                      "BRX chooses correct target and honors "
                      "predicate/fallthrough");
            }
        }
    }
    std::cout << "PASS: GPU direct/table indirect branches, changed tables, "
                 "predicates and index clamp"
              << std::endl;
}

void testClearColor(MTL::Device* device) {
    ClearColorPipelineCache cache(device);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    const std::array formats{MTL::PixelFormatR32Uint, MTL::PixelFormatR32Sint,
                             MTL::PixelFormatR32Float};
    const std::array<u32, 3> values{0xdeadbeefu, 0xff123456u, 0x3f000000u};
    for (usize index = 0; index < formats.size(); ++index) {
        // Writing color attachment 1 exercises explicit output routing too.
        ClearColorPipelineDescriptor descriptor{};
        descriptor.pixel_formats[1] = formats[index];
        descriptor.render_target_id = 1;
        descriptor.mask = 1;
        auto* pipeline = cache.find(descriptor);
        check(pipeline != nullptr, "Integer/float clear pipeline compiles");
        NS_STACK_SCOPED auto texture_descriptor = MTL::TextureDescriptor::alloc()->init();
        texture_descriptor->setPixelFormat(formats[index]);
        texture_descriptor->setWidth(4);
        texture_descriptor->setHeight(4);
        texture_descriptor->setStorageMode(MTL::StorageModeShared);
        texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
        check(texture != nullptr, "Clear texture exists");
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        pass->setRenderTargetArrayLength(1);
        auto* attachment = pass->colorAttachments()->object(1);
        attachment->setTexture(texture);
        attachment->setLoadAction(MTL::LoadActionClear);
        attachment->setStoreAction(MTL::StoreActionStore);
        auto* command = queue->commandBuffer();
        auto* encoder = command->renderCommandEncoder(pass);
        encoder->setRenderPipelineState(pipeline);
        const u32 layer = 0;
        const std::array<u32, 4> color{values[index], 0, 0, 0};
        encoder->setVertexBytes(&layer, sizeof(layer), 0);
        encoder->setFragmentBytes(color.data(), sizeof(color), 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
        command->commit();
        command->waitUntilCompleted();
        check(command->status() == MTL::CommandBufferStatusCompleted, "Typed clear executes");
        std::array<u32, 16> pixels{};
        texture->getBytes(pixels.data(), 16, MTL::Region::Make2D(0, 0, 4, 4), 0);
        for (auto pixel : pixels)
            check(pixel == values[index], "Typed clear preserves raw color bits on attachment 1");
    }
}

// Fullscreen guest shaders can omit Z/W. Exercise rasterization with both
// omitted components and explicit overrides through the real MSL emitter.
void testVertexPositionDefaults(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    for (bool explicit_zw : {false, true}) {
        DecompilerContext context{};
        context.type = ShaderType::Vertex;
        GuestShaderState guest{};
        for (auto& attribute : guest.vertex_attrib_states)
            attribute.is_fixed = true;
        guest.depth_zero_to_one = true;
        ir::Module module;
        ir::Builder builder(module);
        const auto vid =
            ir::Value::createAttrMemory({RZ, SV_VERTEX_ID_BASE, true});
        const float scale = explicit_zw ? 2.0f : 1.0f;
        for (u32 component = 0; component < 2; ++component) {
            const auto condition = builder.opCompareEqual(
                vid, ir::Value::createConstant(component + 1));
            const auto coord = builder.opSelect(
                condition, ir::Value::createConstantF(3.0f * scale),
                ir::Value::createConstantF(-scale));
            builder.opCopy(ir::Value::createAttrMemory(
                               {RZ, SV_POSITION_BASE + component * 4, false},
                               ir::ScalarType::F32),
                           coord);
        }
        if (explicit_zw) {
            builder.opCopy(
                ir::Value::createAttrMemory({RZ, SV_POSITION_BASE + 8, false},
                                            ir::ScalarType::F32),
                ir::Value::createConstantF(0.5f));
            builder.opCopy(
                ir::Value::createAttrMemory({RZ, SV_POSITION_BASE + 12, false},
                                            ir::ScalarType::F32),
                ir::Value::createConstantF(2.0f));
        }
        builder.opCopy(
            ir::Value::createAttrMemory({RZ, SV_USER_IN_OUT_BASE, false},
                                        ir::ScalarType::F32),
            ir::Value::createConstantF(0.25f));
        builder.opExit();
        analyzer::MemoryAnalyzer memory;
        memory.analyze(module);
        std::vector<u8> generated;
        ResourceMapping mapping;
        codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                               generated, mapping);
        emitter.emit(module);
        std::string source(generated.begin(), generated.end());
        source += "\nfragment float4 check_position(float4 p [[position]], "
                  "float4 value [[user(locn0)]]) { "
                  "return float4(p.z,value.y+value.z,value.w,1); }\n";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        if (!library && error)
            std::cerr << error->localizedDescription()->utf8String()
                      << std::endl;
        check(library != nullptr, "Vertex position defaults compile");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("check_position"));
        NS_STACK_SCOPED auto descriptor =
            MTL::RenderPipelineDescriptor::alloc()->init();
        descriptor->setVertexFunction(vertex);
        descriptor->setFragmentFunction(fragment);
        descriptor->colorAttachments()->object(0)->setPixelFormat(
            MTL::PixelFormatRGBA8Unorm);
        NS_STACK_SCOPED auto pipeline =
            device->newRenderPipelineState(descriptor, &error);
        check(pipeline != nullptr, "Vertex position pipeline compiles");
        auto* texture_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA8Unorm, 4, 4, false);
        texture_descriptor->setStorageMode(MTL::StorageModeShared);
        texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* color = pass->colorAttachments()->object(0);
        color->setTexture(texture);
        color->setLoadAction(MTL::LoadActionClear);
        color->setStoreAction(MTL::StoreActionStore);
        color->setClearColor(MTL::ClearColor(0, 0, 0, 1));
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        auto* commands = queue->commandBuffer();
        auto* encoder = commands->renderCommandEncoder(pass);
        encoder->setRenderPipelineState(pipeline);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3));
        encoder->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        check(commands->status() == MTL::CommandBufferStatusCompleted,
              "Vertex position draw completes");
        std::array<u8, 64> pixels{};
        texture->getBytes(pixels.data(), 16, MTL::Region::Make2D(0, 0, 4, 4),
                          0);
        for (usize pixel = 0; pixel < 16; ++pixel) {
            check(pixels[pixel * 4] == (explicit_zw ? 64 : 0) &&
                      pixels[pixel * 4 + 1] == 0 &&
                      pixels[pixel * 4 + 2] == 255,
                  "XY-only triangle and partial varying rasterize with "
                  "defaults; explicit Z/W remain intact");
        }
    }
    std::cout << "PASS: XY-only vertex positions rasterize; explicit Z/W "
                 "overrides survive\n";
}

// Maxwell's SHFL.BFLY + FSWZADD patterns implement positive X/Y derivatives.
// Exercise the emitter on real fragment quads, including lane-dependent signs.
void testQuadDerivatives(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    DecompilerContext context{};
    context.type = ShaderType::Fragment;
    GuestShaderState guest{};
    guest.color_target_data_types[0] = MTL::DataTypeFloat;
    ir::Module module;
    ir::Builder builder(module);
    for (u32 axis = 0; axis < 2; ++axis) {
        const auto value = ir::Value::createAttrMemory(
            {RZ, SV_POSITION_BASE + axis * 4, true}, ir::ScalarType::F32);
        const auto neighbour =
            builder.opBitCast(builder.opQuadShuffleXor(
                                  builder.opBitCast(value, ir::ScalarType::U32),
                                  ir::Value::createConstantU(1u << axis)),
                              ir::ScalarType::F32);
        const auto derivative =
            builder.opSwizzleAdd(neighbour, value, axis == 0 ? 0x99 : 0xa5);
        builder.opCopy(ir::Value::createRegister(static_cast<u8>(axis),
                                                 ir::ScalarType::F32),
                       derivative);
    }
    for (u32 component = 2; component < 4; ++component) {
        const auto value = builder.opSwizzleAdd(ir::Value::createConstantF(7),
                                                ir::Value::createConstantF(3),
                                                component == 2 ? 0 : 0xff);
        builder.opCopy(ir::Value::createRegister(static_cast<u8>(component),
                                                 ir::ScalarType::F32),
                       value);
    }
    builder.opExit();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += "\nvertex float4 quad_vertex(uint id [[vertex_id]]) { return "
              "float4(id==1?3.0:-1.0,id==2?3.0:-1.0,0,1); }\n";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << '\n';
    check(library != nullptr, "Quad derivative shader compiles");
    NS_STACK_SCOPED auto vertex =
        library->newFunction(toNsString("quad_vertex"));
    NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("main_"));
    NS_STACK_SCOPED auto pd = MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vertex);
    pd->setFragmentFunction(fragment);
    pd->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatRGBA32Float);
    NS_STACK_SCOPED auto pipeline = device->newRenderPipelineState(pd, &error);
    check(pipeline != nullptr, "Quad derivative pipeline compiles");
    auto* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA32Float, 4, 4, false);
    td->setStorageMode(MTL::StorageModeShared);
    td->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto texture = device->newTexture(td);
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(texture);
    color->setLoadAction(MTL::LoadActionClear);
    color->setStoreAction(MTL::StoreActionStore);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->renderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                            NS::UInteger(3));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    check(commands->status() == MTL::CommandBufferStatusCompleted,
          "Quad derivative draw completes");
    std::array<float, 64> pixels{};
    texture->getBytes(pixels.data(), 64, MTL::Region::Make2D(0, 0, 4, 4), 0);
    for (u32 i = 0; i < 16; ++i) {
        check(pixels[i * 4] == 1.0f && pixels[i * 4 + 1] == 1.0f,
              "SHFL/FSWZADD produce positive derivatives in every quad lane");
        check(pixels[i * 4 + 2] == 10.0f && pixels[i * 4 + 3] == 3.0f,
              "FSWZADD addition and move modes remain intact");
    }
    std::cout << "PASS: fragment quad derivatives and FSWZADD add/move modes\n";
}

// Fragment depth follows packed color exports and a reserved sample-mask slot.
// Read the native depth attachment back, rather than only checking emitted
// text.
void testFragmentDepth(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    for (u32 color_count : {0u, 1u, 6u}) {
        for (bool export_depth : {false, true}) {
            std::vector<u64> words(10, 0);
            words[0] = 3u << 5;
            const u32 mask = (1u << (color_count * 4)) - 1;
            words[9] = u64(mask) | (u64(export_depth ? 2 : 0) << 32);
            // Set the exported depth to 0.75; rasterized depth is 0.25.
            words.push_back(0);
            words.push_back(0x010000000000f000ull |
                            (color_count * 4 + 1) |
                            (u64(std::bit_cast<u32>(0.75f)) << 20));
            words.push_back(0xe30000000007000full);
            words.push_back(0x50b0000000070f00ull);
            std::vector<u8> bytes(words.size() * 8);
            std::memcpy(bytes.data(), words.data(), bytes.size());
            ztd::io::MemoryStream stream(bytes);
            GuestShaderState guest{};
            for (u32 i = 0; i < color_count; ++i)
                guest.color_target_data_types[i] = MTL::DataTypeFloat;
            ShaderBackend backend;
            std::vector<u8> generated;
            ResourceMapping mapping;
            decompile(stream, ShaderType::Fragment, guest, backend,
                      generated, mapping);
            std::string source(generated.begin(), generated.end());
            source += R"(
vertex float4 depth_vertex(uint id [[vertex_id]]) {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.25, 1.0);
}
)";
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                device->newLibrary(toNsString(source), nullptr, &error);
            if (!library && error)
                std::cerr << error->localizedDescription()->utf8String() << '\n';
            check(library != nullptr, "Fragment depth shader compiles");
            NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("depth_vertex"));
            NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
            descriptor->setVertexFunction(vertex);
            descriptor->setFragmentFunction(fragment);
            descriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
            // Only depth is attached: color export count still selects its register.
            NS_STACK_SCOPED auto pipeline = device->newRenderPipelineState(descriptor, &error);
            check(pipeline != nullptr, "Fragment depth pipeline compiles");
            auto* td = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormatDepth32Float, 4, 4, false);
            td->setStorageMode(MTL::StorageModePrivate);
            td->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto depth = device->newTexture(td);
            NS_STACK_SCOPED auto readback = device->newBuffer(1024, MTL::ResourceStorageModeShared);
            DepthStencilStateCache depth_cache(device);
            NS_STACK_SCOPED auto ds = depth_cache.create({
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = hydra::hw::tegra_x1::gpu::engines::CompareOp::Always,
            });
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            pass->depthAttachment()->setTexture(depth);
            pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
            pass->depthAttachment()->setStoreAction(MTL::StoreActionStore);
            pass->depthAttachment()->setClearDepth(0);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline);
            encoder->setDepthStencilState(ds);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            encoder->endEncoding();
            auto* blit = commands->blitCommandEncoder();
            blit->copyFromTexture(depth, 0, 0, MTL::Origin(0, 0, 0),
                                  MTL::Size(4, 4, 1), readback, 0, 256, 1024);
            blit->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted,
                  "Fragment depth draw completes");
            for (u32 y = 0; y < 4; ++y) {
                const auto* row = reinterpret_cast<const float*>(
                    static_cast<const u8*>(readback->contents()) + y * 256);
                for (u32 x = 0; x < 4; ++x)
                    check(row[x] == (export_depth ? 0.75f : 0.25f),
                          "Fragment depth export reaches native depth attachment");
            }
        }
    }
    std::cout << "PASS: fragment depth with zero, one and six color exports\n";
}

// UE4 renders volume textures (color grading LUT, translucency lighting) with
// a geometry shader that only forwards the vertex shader's layer index. Find
// that attribute and render each instance into its own 3D slice.
void testGeometryLayerForwarding(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    const auto make_code = [](std::initializer_list<u64> insts) {
        std::vector<u64> words(0x50 / 8, 0);
        usize slot = 0;
        for (const auto inst : insts) {
            if (slot++ % 3 == 0)
                words.push_back(0); // scheduling
            words.push_back(inst);
        }
        words.push_back(0xe30000000007000full); // exit
        words.push_back(0);
        std::vector<u8> bytes(words.size() * 8);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        return bytes;
    };
    const auto ald = [](u64 dst, u64 addr, u64 size) {
        return 0xefd8000000000000ull | dst | (0xffull << 8) | (addr << 20) |
               (0xffull << 39) | (size << 47);
    };
    const auto ast = [](u64 src, u64 addr, u64 size) {
        return 0xeff0000000000000ull | src | (0xffull << 8) | (addr << 20) |
               (0xffull << 39) | (size << 47);
    };
    const auto mov32i = [](u64 dst, u64 imm) {
        return 0x010000000000f000ull | dst | (imm << 20);
    };
    check(findGeometryLayerSource(make_code({ald(0, 0x90, 0),
                                             ast(0, 0x64, 0)})) == 0x90u,
          "Layer forwarded from an input attribute");
    check(findGeometryLayerSource(make_code({ald(4, 0x90, 3),
                                             ast(4, 0x60, 3)})) == 0x94u,
          "Layer forwarded through a vector store");
    check(!findGeometryLayerSource(make_code(
              {ald(0, 0x90, 0), mov32i(0, 3), ast(0, 0x64, 0)})),
          "Computed layer is not forwarded");
    check(!findGeometryLayerSource(make_code({ald(0, 0x90, 0)})),
          "No layer write");

    // Vertex shader: full screen triangle, layer index = instance ID
    constexpr u32 LAYER_ADDR = SV_USER_IN_OUT_BASE + 0x10;
    constexpr u32 SLICES = 4;
    DecompilerContext context{};
    context.type = ShaderType::Vertex;
    GuestShaderState guest{};
    for (auto& attribute : guest.vertex_attrib_states)
        attribute.is_fixed = true;
    guest.depth_zero_to_one = true;
    guest.layer_output_addr = LAYER_ADDR;
    ir::Module module;
    ir::Builder builder(module);
    const auto vid = ir::Value::createAttrMemory({RZ, SV_VERTEX_ID_BASE, true});
    for (u32 component = 0; component < 2; ++component) {
        const auto condition = builder.opCompareEqual(
            vid, ir::Value::createConstant(component + 1));
        const auto coord =
            builder.opSelect(condition, ir::Value::createConstantF(3.0f),
                             ir::Value::createConstantF(-1.0f));
        builder.opCopy(ir::Value::createAttrMemory(
                           {RZ, SV_POSITION_BASE + component * 4, false},
                           ir::ScalarType::F32),
                       coord);
    }
    builder.opCopy(
        ir::Value::createAttrMemory({RZ, LAYER_ADDR, false}),
        ir::Value::createAttrMemory({RZ, SV_INSTANCE_ID_BASE, true}));
    builder.opExit();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += "\nfragment float4 check_layer(uint layer "
              "[[render_target_array_index]]) { "
              "return float4(1, float(layer) / 255.0, 0, 1); }\n";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "Layered vertex shader compiles");
    NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
    NS_STACK_SCOPED auto fragment =
        library->newFunction(toNsString("check_layer"));
    NS_STACK_SCOPED auto descriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertex);
    descriptor->setFragmentFunction(fragment);
    descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
    descriptor->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatRGBA8Unorm);
    NS_STACK_SCOPED auto pipeline =
        device->newRenderPipelineState(descriptor, &error);
    check(pipeline != nullptr, "Layered pipeline compiles");

    NS_STACK_SCOPED auto texture_descriptor =
        MTL::TextureDescriptor::alloc()->init();
    texture_descriptor->setTextureType(MTL::TextureType3D);
    texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    texture_descriptor->setWidth(4);
    texture_descriptor->setHeight(4);
    texture_descriptor->setDepth(SLICES);
    texture_descriptor->setStorageMode(MTL::StorageModeShared);
    texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(texture);
    color->setLoadAction(MTL::LoadActionClear);
    color->setStoreAction(MTL::StoreActionStore);
    color->setClearColor(MTL::ClearColor(0, 0, 0, 0));
    pass->setRenderTargetArrayLength(SLICES);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->renderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    // Instanced draws are split into one draw per instance, like the 3D engine
    for (u32 instance = 0; instance < SLICES; ++instance)
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3), NS::UInteger(1),
                                NS::UInteger(instance));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    check(commands->status() == MTL::CommandBufferStatusCompleted,
          "Layered draw completes");
    std::array<u8, 64 * SLICES> pixels{};
    texture->getBytes(pixels.data(), 16, 64, MTL::Region::Make3D(0, 0, 0, 4, 4, SLICES),
                      0, 0);
    for (u32 slice = 0; slice < SLICES; ++slice) {
        for (usize pixel = 0; pixel < 16; ++pixel) {
            const auto* p = &pixels[(slice * 16 + pixel) * 4];
            check(p[0] == 255 && p[1] == slice,
                  "Each instance renders into the slice it selects");
        }
    }
    std::cout << "PASS: forwarded geometry shader layers render into their "
                 "3D slices\n";
}

// LEA and LEA.HI scale a (64-bit for HI) offset and add a base. Execute every
// variant on the GPU and compare with CPU reference arithmetic.
void testLea(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    struct Case {
        bool hi;
        bool immediate;
        u32 shift;
        bool neg;
    };
    constexpr u32 IMM = 0x123;
    std::vector<Case> cases;
    std::vector<u64> words;
    const auto append = [&](u64 x) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(x);
    };
    for (u32 shift : {0u, 1u, 5u, 31u}) {
        for (bool neg : {false, true}) {
            const u64 n = static_cast<u64>(neg);
            const u64 common = (1ull << 8) | (7ull << 16) | (7ull << 48);
            // LEA R, C, I
            append(0x5bd0000000000000ull | (16ull + cases.size()) | common |
                   (2ull << 20) | (static_cast<u64>(shift) << 39) | (n << 45));
            cases.push_back({false, false, shift, neg});
            append(0x4bd0000000000000ull | (16ull + cases.size()) | common |
                   (2ull << 34) | (2ull << 20) |
                   (static_cast<u64>(shift) << 39) | (n << 45));
            cases.push_back({false, false, shift, neg});
            append(0x36d0000000000000ull | (16ull + cases.size()) | common |
                   (static_cast<u64>(IMM) << 20) |
                   (static_cast<u64>(shift) << 39) | (n << 45));
            cases.push_back({false, true, shift, neg});
            // LEA.HI R, C
            append(0x5bd8000000000000ull | (16ull + cases.size()) | common |
                   (2ull << 20) | (static_cast<u64>(shift) << 28) |
                   (n << 37) | (3ull << 39));
            cases.push_back({true, false, shift, neg});
            append(0x1800000000000000ull | (16ull + cases.size()) | common |
                   (2ull << 34) | (2ull << 20) | (3ull << 39) |
                   (static_cast<u64>(shift) << 51) | (n << 56));
            cases.push_back({true, false, shift, neg});
        }
    }
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    ResourceMapping mapping;
    std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    const auto count = static_cast<u32>(cases.size());
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += fmt::format(
        "\nkernel void check_lea(device uint* out [[buffer(0)]], constant "
        "Reg* in [[buffer(1)]]) {{ State state; state.c2=in; "
        "state.r[1]=in[1]; state.r[2]=in[2]; state.r[3]=in[3]; "
        "run_program(state); for(uint i=0;i<{};i++) "
        "out[i]=state.r[16+i]._u32; }}",
        count);
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "LEA guest instructions compile");
    NS_STACK_SCOPED auto function =
        library->newFunction(toNsString("check_lea"));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "LEA pipeline exists");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    NS_STACK_SCOPED auto output =
        device->newBuffer(count * 4, MTL::ResourceStorageModeShared);
    for (const auto [lo, hi, base] :
         {std::array<u32, 3>{0x89abcdefu, 0x01234567u, 0x1000u},
          std::array<u32, 3>{0u, 0x80000001u, 0xfffffff0u},
          std::array<u32, 3>{0xffffffffu, 0xffffffffu, 7u}}) {
        const std::array<u32, 4> in{0, lo, base, hi};
        auto* commands = queue->commandBuffer();
        auto* encoder = commands->computeCommandEncoder();
        encoder->setComputePipelineState(pipeline);
        encoder->setBuffer(output, 0, 0);
        encoder->setBytes(in.data(), sizeof(in), 1);
        encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        check(commands->status() == MTL::CommandBufferStatusCompleted,
              "LEA executes");
        const auto* result = static_cast<const u32*>(output->contents());
        for (u32 index = 0; index < count; ++index) {
            const auto& c = cases[index];
            const u32 b = c.immediate ? IMM : base;
            u32 expected;
            if (c.hi) {
                u64 a = (static_cast<u64>(hi) << 32) | lo;
                if (c.neg)
                    a = ~a + 1;
                expected = static_cast<u32>(a >> (32 - c.shift)) + b;
            } else {
                const u32 a = c.neg ? ~lo + 1 : lo;
                expected = (a << c.shift) + b;
            }
            if (result[index] != expected)
                throw std::runtime_error(fmt::format(
                    "LEA case {} (hi {}, shift {}, neg {}): {:x} != {:x}",
                    index, c.hi, c.shift, c.neg, result[index], expected));
        }
    }
    std::cout << "PASS: GPU LEA/LEA.HI register, constant and immediate "
                 "variants with negation"
              << std::endl;
}

// XMAD.CC feeds its carry to XMAD.X (64-bit multiply chains), and I2I.CC
// sets the zero/sign flags that CSETP reads.
void testConditionCodes(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    std::vector<u64> words;
    const auto append = [&](u64 x) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(x);
    };
    // XMAD.CC R16, R1, R2, R3 (low halves)
    append(0x5b00000000000000ull | 16 | (1ull << 8) | (7ull << 16) |
           (2ull << 20) | (3ull << 39) | (1ull << 47));
    // XMAD.X R17, R4, R2, R5
    append(0x5b00000000000000ull | 17 | (4ull << 8) | (7ull << 16) |
           (2ull << 20) | (5ull << 39) | (1ull << 38));
    // I2I.CC RZ, R6 (the encoding Crash Bandicoot 4 uses, register variant)
    append((0x4ce0801000470affull & ~(0x7ffffull << 20) &
            ~0xfff8000000000000ull) |
           0x5ce0000000000000ull | (6ull << 20));
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    ResourceMapping mapping;
    std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += "\nkernel void check_cc(device uint* out [[buffer(0)]], constant "
              "Reg* in [[buffer(1)]]) { State state; for(uint i=1;i<7;i++) "
              "state.r[i]=in[i]; for(uint i=0;i<4;i++) state.cc[i]=true; "
              "run_program(state); out[0]=state.r[16]._u32; "
              "out[1]=state.r[17]._u32; for(uint i=0;i<4;i++) "
              "out[2+i]=state.cc[i]?1u:0u; }\n";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "Condition code instructions compile");
    NS_STACK_SCOPED auto function =
        library->newFunction(toNsString("check_cc"));
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "Condition code pipeline exists");
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    NS_STACK_SCOPED auto output =
        device->newBuffer(6 * 4, MTL::ResourceStorageModeShared);
    for (const u32 value : {0u, 5u, 0x80000000u}) {
        // 0xffff * 0xffff + 0x2ffff carries out of 32 bits
        const std::array<u32, 7> in{0, 0xffff, 0xffff, 0x2ffff, 0, 7, value};
        auto* commands = queue->commandBuffer();
        auto* encoder = commands->computeCommandEncoder();
        encoder->setComputePipelineState(pipeline);
        encoder->setBuffer(output, 0, 0);
        encoder->setBytes(in.data(), sizeof(in), 1);
        encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        const auto* result = static_cast<const u32*>(output->contents());
        check(result[0] == 0x00010000u, "XMAD.CC low word");
        check(result[1] == 8u, "XMAD.X adds the carry");
        check(result[2] == (value == 0 ? 1u : 0u), "I2I.CC zero flag");
        check(result[3] == (value >> 31), "I2I.CC sign flag");
        check(result[4] == 0 && result[5] == 0,
              "I2I.CC clears carry and overflow");
    }
    std::cout << "PASS: GPU XMAD.CC/XMAD.X carry chain and I2I.CC flags\n";
}

// Execute compare/select guest opcodes through the decoder and MSL emitter.
// Include integer signedness, float NaNs/FTZ, operand locations and predicates.
void testCompareSelect(MTL::Device* device, bool floating) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    const std::array<u64, 4> opcodes =
        floating ? std::array<u64, 4>{0x5ba0000000000000, 0x4ba0000000000000,
                                      0x53a0000000000000, 0x36a0000000000000}
                 : std::array<u64, 4>{0x5b40000000000000, 0x4b40000000000000,
                                      0x5340000000000000, 0x3640000000000000};
    const std::vector<u32> operands =
        floating
            ? std::vector<u32>{0xbf800000u, 0u,          0x80000000u,
                               0x3f800000u, 0x7f800000u, 0xff800000u,
                               0x7fc12345u, 1u,          0x80000001u}
            : std::vector<u32>{0xffffffffu, 0u, 1u, 0x80000000u, 0x7fffffffu};
    const u32 comparison_count = floating ? 16 : 8;
    const u32 unconditional_count = comparison_count * 2;
    const u32 result_count = unconditional_count + 2;
    const auto compiler_directory =
        std::filesystem::temp_directory_path() /
        fmt::format("hydra-compare-test-{}-{}", getpid(), floating);
    std::filesystem::remove_all(compiler_directory);
    auto compiler =
        std::make_unique<ShaderLibraryCache>(device, compiler_directory);
    constexpr u32 selected = 0x7fc12345u;
    constexpr u32 sentinel = 0x12345678u;
    for (usize variant = 0; variant < opcodes.size(); ++variant) {
        std::vector<u64> words;
        const auto append = [&](u64 instruction) {
            if (words.size() % 4 == 0)
                words.push_back(0); // Maxwell scheduling word
            words.push_back(instruction);
        };
        for (u32 index = 0; index < result_count; ++index) {
            const u32 op =
                index < unconditional_count ? index % comparison_count : 2;
            const u32 sign =
                index < unconditional_count ? index / comparison_count : 1;
            u64 instruction = opcodes[variant] | (16u + index) | (1ull << 8) |
                              (static_cast<u64>(sign) << (floating ? 47 : 48)) |
                              (static_cast<u64>(op) << (floating ? 48 : 49));
            if (index < unconditional_count)
                instruction |= 7ull << 16;
            else if (index == unconditional_count + 1)
                instruction |= 1ull << 19; // inverted P0
            if (variant == 0)
                instruction |= (2ull << 20) | (3ull << 39);
            else if (variant == 1)
                instruction |= (2ull << 34) | (3ull << 39); // c[2][0], R3
            else if (variant == 2)
                instruction |=
                    (1ull << 20) | (2ull << 34) | (2ull << 39); // R2, c[2][4]
            else
                instruction |= ((floating ? 0x3fc00ull : 0x7fff9ull) << 20) |
                               (1ull << 56) | (3ull << 39); // -7, R3
            append(instruction);
        }
        append(0xe30000000007000f); // EXIT PT
        std::vector<u8> bytes(words.size() * sizeof(u64));
        std::memcpy(bytes.data(), words.data(), bytes.size());
        ztd::io::MemoryStream stream(bytes);
        DecompilerContext context{};
        context.type = ShaderType::Compute;
        ir::Module module;
        ir::Builder builder(module);
        decoder::Decoder decoder({context, &stream, builder});
        decoder.decode();
        analyzer::MemoryAnalyzer memory;
        memory.analyze(module);
        GuestShaderState guest{};
        std::vector<u8> generated;
        ResourceMapping mapping;
        codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                               generated, mapping);
        emitter.emit(module);
        std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
        source +=
            "\nkernel void check_icmp(device uint* output [[buffer(0)]], "
            "constant Reg* constants [[buffer(1)]], uint tid "
            "[[thread_position_in_grid]]) {\n"
            "State state; state.r[1]._u32=0x7fc12345u; "
            "state.r[2]._u32=0xdeadbeefu; "
            "state.r[3]._u32=constants[1]._u32; state.p[0]=(tid & 1u)!=0u;\n";
        if (variant == 1 || variant == 2)
            source += "state.c2=constants;\n";
        source +=
            fmt::format("for(uint i=16;i<{};i++) state.r[i]._u32=0x12345678u;\n"
                        "run_program(state); for(uint i=0;i<{};i++) "
                        "output[tid*{}+i]=state.r[16+i]._u32; }}\n",
                        16 + result_count, result_count, result_count);
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library = compiler->get(source, &error);
        if (!library && error)
            std::cerr << error->localizedDescription()->utf8String()
                      << std::endl;
        check(library != nullptr, "Guest compare/select compiles as MSL");
        NS_STACK_SCOPED auto function =
            library->newFunction(toNsString("check_icmp"));
        NS_STACK_SCOPED auto pipeline =
            device->newComputePipelineState(function, &error);
        NS_STACK_SCOPED auto output = device->newBuffer(
            2 * result_count * sizeof(u32), MTL::ResourceStorageModeShared);
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        for (u32 operand : operands) {
            const std::array<u32, 2> constants{0xdeadbeefu, operand};
            NS_STACK_SCOPED auto inputs =
                device->newBuffer(constants.data(), sizeof(constants),
                                  MTL::ResourceStorageModeShared);
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->setBuffer(inputs, 0, 1);
            encoder->dispatchThreads(MTL::Size(2, 1, 1), MTL::Size(2, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted,
                  "Guest compare/select executes");
            const auto* result = static_cast<const u32*>(output->contents());
            for (u32 lane = 0; lane < 2; ++lane) {
                for (u32 index = 0; index < result_count; ++index) {
                    const bool sign = index >= comparison_count;
                    const u32 op = index < unconditional_count
                                       ? index % comparison_count
                                       : 2;
                    bool comparison;
                    if (floating) {
                        float value = std::bit_cast<float>(operand);
                        if (sign && std::fpclassify(value) == FP_SUBNORMAL)
                            value = std::copysign(0.0f, value);
                        const bool nan = std::isnan(value);
                        const std::array<bool, 16> comparisons{
                            false,
                            value < 0,
                            value == 0,
                            value <= 0,
                            value > 0,
                            !nan && value != 0,
                            value >= 0,
                            !nan,
                            nan,
                            nan || value < 0,
                            nan || value == 0,
                            nan || value <= 0,
                            nan || value > 0,
                            value != 0,
                            nan || value >= 0,
                            true};
                        comparison = comparisons[op];
                    } else {
                        const auto value =
                            sign ? static_cast<i64>(std::bit_cast<i32>(operand))
                                 : static_cast<i64>(operand);
                        const std::array<bool, 8> comparisons{
                            false,     value < 0,  value == 0, value <= 0,
                            value > 0, value != 0, value >= 0, true};
                        comparison = comparisons[op];
                    }
                    const bool execute =
                        index < unconditional_count ||
                        (index == unconditional_count ? lane == 1 : lane == 0);
                    const u32 alternative =
                        variant == 3 ? (floating ? 0xbfc00000u : 0xfffffff9u)
                                     : 0xdeadbeefu;
                    const u32 expected =
                        execute ? (comparison ? selected : alternative)
                                : sentinel;
                    if (result[lane * result_count + index] != expected)
                        std::cerr << fmt::format(
                            "Compare failure float={} variant={} operand={:#x} "
                            "lane={} index={} got={:#x} expected={:#x}\n",
                            floating, variant, operand, lane, index,
                            result[lane * result_count + index], expected);
                    check(result[lane * result_count + index] == expected,
                          "Guest compare/select preserves selected bits and "
                          "predicates");
                }
            }
        }
    }
    compiler.reset();
    std::filesystem::remove_all(compiler_directory);
    std::cout << "PASS: guest "
              << (floating ? "FCMP (NaN/FTZ)" : "ICMP (signed/unsigned)")
              << " register, constant, immediate and predicated GPU results\n";
}

// ISET/FSET write 0xffffffff for true without .BF (games use the result as a
// mask or as -1, e.g. UE4's AO composite negates it before I2F) and 1.0f with
// .BF. Writing 1 turned UE4's ambient occlusion factor into -4e9.
void testSetRegisterResults(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    std::vector<u64> words;
    auto append = [&](u64 instruction) {
        if (words.size() % 4 == 0)
            words.push_back(0x001f8000fc0007e0ull); // scheduling word
        words.push_back(instruction);
    };
    const u64 iset = 0x5b50000000000000ull | (7ull << 16) | (7ull << 39) |
                     (255ull << 20) | (1ull << 8); // ISET R1, RZ, PT
    append(iset | (5ull << 49) | 16);              // NE -> R16
    append(iset | (2ull << 49) | 17);              // EQ -> R17
    append(iset | (5ull << 49) | (1ull << 44) | 18); // NE.BF -> R18
    const u64 fset = 0x5800000000000000ull | (7ull << 16) | (7ull << 39) |
                     (255ull << 20) | (2ull << 8); // FSET R2, RZ, PT
    append(fset | (4ull << 48) | 19);              // GT -> R19
    append(fset | (1ull << 48) | 20);              // LT -> R20
    append(fset | (4ull << 48) | (1ull << 52) | 21); // GT.BF -> R21
    append(0xe30000000007000full);                 // EXIT PT
    std::vector<u8> bytes(words.size() * sizeof(u64));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end()); source += runProgramSource(module);
    source += "\nkernel void check_set(device uint* output [[buffer(0)]]) {\n"
              "State state; state.r[1]._u32=7u; state.r[2]._f32=2.5f;\n"
              "for(uint i=16;i<22;i++) state.r[i]._u32=0x12345678u;\n"
              "run_program(state);\n"
              "for(uint i=0;i<6;i++) output[i]=state.r[16+i]._u32; }\n";
    const auto compiler_directory =
        std::filesystem::temp_directory_path() / "hydra-set-register-tests";
    std::filesystem::remove_all(compiler_directory);
    auto compiler = std::make_unique<ShaderLibraryCache>(device, compiler_directory);
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library = compiler->get(source, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << std::endl;
    check(library != nullptr, "Guest ISET/FSET compiles as MSL");
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("check_set"));
    NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
    NS_STACK_SCOPED auto output =
        device->newBuffer(6 * sizeof(u32), MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(output, 0, 0);
    encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    check(commands->status() == MTL::CommandBufferStatusCompleted,
          "Guest ISET/FSET executes");
    const auto* result = static_cast<const u32*>(output->contents());
    const std::array<u32, 6> expected{0xffffffffu, 0u, 0x3f800000u,
                                      0xffffffffu, 0u, 0x3f800000u};
    for (u32 i = 0; i < expected.size(); ++i) {
        if (result[i] != expected[i])
            std::cerr << fmt::format("Set result {} got={:#x} expected={:#x}\n",
                                     i, result[i], expected[i]);
        check(result[i] == expected[i],
              "ISET/FSET write 0xffffffff (or 1.0 with .BF) for true");
    }
    compiler.reset();
    std::filesystem::remove_all(compiler_directory);
    std::cout << "PASS: guest ISET/FSET integer mask and .BF float results\n";
}

// Execute TEX.LB/LBA through the decoder and MSL emitter with colored mip
// levels. This distinguishes implicit sampling, bias and sampler LOD clamps.
void testTextureLodSelection() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    metal::Renderer mip_renderer;
    auto* mip_device = mip_renderer.getDevice();
    auto* mip_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA8Unorm, 4, 4, true);
    mip_descriptor->setStorageMode(MTL::StorageModeShared);
    mip_descriptor->setUsage(MTL::TextureUsageShaderRead);
    NS_STACK_SCOPED auto mip_texture = mip_device->newTexture(mip_descriptor);
    for (u32 mip = 0; mip < 3; ++mip) {
        const u32 side = 4u >> mip;
        std::vector<u8> texels(side * side * 4, 0);
        for (u32 pixel = 0; pixel < side * side; ++pixel) {
            texels[pixel * 4 + mip] = 255;
            texels[pixel * 4 + 3] = 255;
        }
        mip_texture->replaceRegion(MTL::Region::Make2D(0, 0, side, side), mip,
                                   texels.data(), side * 4);
    }
    for (const auto mode : {decoder::Lod::Lb, decoder::Lod::Lba}) {
        for (u32 scenario = 0; scenario < 10; ++scenario) {
            const float bias_value =
                scenario < 3 ? float(int(scenario) * 2 - 1) : scenario == 9 ? 2.0f : 0.0f;
            const float clamp = scenario == 3   ? 1.0f
                                : scenario == 4 ? 2.0f
                                                : 1.5f;
            const SamplerDescriptor sampler_descriptor{
                .min_filter = MTL::SamplerMinMagFilterNearest,
                .mag_filter = MTL::SamplerMinMagFilterNearest,
                .mip_filter = MTL::SamplerMipFilterLinear,
                .address_mode_s = MTL::SamplerAddressModeClampToEdge,
                .address_mode_t = MTL::SamplerAddressModeClampToEdge,
                .address_mode_r = MTL::SamplerAddressModeClampToEdge,
                .depth_compare_op =
                    hydra::hw::tegra_x1::gpu::engines::CompareOp::Invalid,
                .border_color_u = simd::uint4{0, 0, 0, 0},
                .min_lod = scenario < 3 || scenario >= 6 ? 0.0f : clamp,
                .max_lod = scenario < 3 || scenario >= 6 ? 15.0f : clamp,
                .lod_bias = scenario == 8 ? 1.0f : scenario == 9 ? -1.0f : 0.0f,
            };
            auto* selected_sampler =
                mip_renderer.getSamplerCache().find(sampler_descriptor);
            auto other_descriptor = sampler_descriptor;
            other_descriptor.min_lod = 0.25f;
            check(mip_renderer.getSamplerCache().find(other_descriptor) !=
                      selected_sampler,
                  "Sampler cache distinguishes the minimum LOD");
            other_descriptor = sampler_descriptor;
            other_descriptor.max_lod = 20.0f;
            check(mip_renderer.getSamplerCache().find(other_descriptor) !=
                      selected_sampler,
                  "Sampler cache distinguishes the maximum LOD");
            DecompilerContext context{};
            context.type = ShaderType::Fragment;
            GuestShaderState guest{};
            guest.color_target_data_types[0] = MTL::DataTypeFloat;
            ir::Module module;
            ir::Builder builder(module);
            for (u32 axis = 0; axis < 2; ++axis) {
                const auto position = ir::Value::createAttrMemory(
                    {RZ, SV_POSITION_BASE + axis * 4, true},
                    ir::ScalarType::F32);
                builder.opCopy(
                    ir::Value::createRegister(static_cast<reg_t>(axis),
                                              ir::ScalarType::F32),
                    builder.opMultiply(position,
                                       ir::Value::createConstantF(0.25f)));
            }
            builder.opCopy(ir::Value::createRegister(2, ir::ScalarType::F32),
                           ir::Value::createConstantF(bias_value));
            decoder::DecoderContext dc{context, nullptr, builder};
            if (scenario == 6) {
                for (u32 index = 0; index < 4; ++index)
                    builder.opCopy(
                        ir::Value::createRegister(static_cast<reg_t>(2 + index),
                                                  ir::ScalarType::F32),
                        ir::Value::createConstantF(
                            index == 0 || index == 3 ? 0.5f : 0.0f));
                decoder::emitTxd(dc, std::bit_cast<decoder::InstTxd>(
                                         0xde38000000000000ull | 4ull |
                                         (7ull << 16) | (2ull << 20) |
                                         (2ull << 28) | (15ull << 31) |
                                         (7ull << 36)));
            } else {
                const auto instruction = std::bit_cast<decoder::InstTex>(
                    0xc038000000000000ull | 4ull | (7ull << 16) | (2ull << 20) |
                    (2ull << 28) | (15ull << 31) | (7ull << 36) |
                    (static_cast<u64>(scenario == 7 ? decoder::Lod::Lz : mode)
                     << 55));
                decoder::emitTex(dc, instruction);
            }
            for (u32 channel = 0; channel < 4; ++channel)
                builder.opCopy(
                    ir::Value::createRegister(static_cast<reg_t>(channel),
                                              ir::ScalarType::F32),
                    ir::Value::createRegister(static_cast<reg_t>(4 + channel),
                                              ir::ScalarType::F32));
            builder.opExit();
            analyzer::MemoryAnalyzer memory;
            memory.analyze(module);
            for (const auto& [binding, texture_type] : memory.getTextures()) {
                (void)texture_type;
                guest.texture_lod_bias[binding] = sampler_descriptor.lod_bias;
            }
            std::vector<u8> generated;
            ResourceMapping mapping;
            codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                                   generated, mapping);
            emitter.emit(module);
            std::string source(generated.begin(), generated.end());
            source +=
                "\nvertex float4 mip_vertex(uint id [[vertex_id]]) { return "
                "float4(id==1?3.0:-1.0,id==2?3.0:-1.0,0,1); }\n";
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                mip_device->newLibrary(toNsString(source), nullptr, &error);
            if (!library && error)
                std::cerr << error->localizedDescription()->utf8String()
                          << '\n';
            check(library != nullptr, "Decoded biased texture sample compiles");
            NS_STACK_SCOPED auto vertex =
                library->newFunction(toNsString("mip_vertex"));
            NS_STACK_SCOPED auto fragment =
                library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto pipeline_descriptor =
                MTL::RenderPipelineDescriptor::alloc()->init();
            pipeline_descriptor->setVertexFunction(vertex);
            pipeline_descriptor->setFragmentFunction(fragment);
            pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
                MTL::PixelFormatRGBA8Unorm);
            NS_STACK_SCOPED auto pipeline =
                mip_device->newRenderPipelineState(pipeline_descriptor, &error);
            check(pipeline != nullptr, "Biased mip pipeline compiles");
            auto* output_descriptor =
                MTL::TextureDescriptor::texture2DDescriptor(
                    MTL::PixelFormatRGBA8Unorm, 4, 4, false);
            output_descriptor->setStorageMode(MTL::StorageModeShared);
            output_descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto output_texture =
                mip_device->newTexture(output_descriptor);
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(output_texture);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            NS_STACK_SCOPED auto queue = mip_device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline);
            const auto slot = mapping.textures.at(7);
            encoder->setFragmentTexture(mip_texture, slot);
            encoder->setFragmentSamplerState(
                static_cast<metal::Sampler*>(selected_sampler)->getSampler(),
                slot);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted,
                  "Mip selection render completes");
            std::array<u8, 64> colors{};
            output_texture->getBytes(colors.data(), 16,
                                     MTL::Region::Make2D(0, 0, 4, 4), 0);
            const float selected_lod = scenario >= 8 ? 1.0f
                                       : scenario == 6   ? 1.0f
                                       : scenario == 7 ? 0.0f
                                       : scenario < 3
                                           ? std::clamp(bias_value, 0.0f, 2.0f)
                                           : clamp;
            for (u32 pixel = 0; pixel < 16; ++pixel) {
                for (u32 channel = 0; channel < 3; ++channel) {
                    const int expected = static_cast<int>(
                        255.0f *
                        std::max(0.0f, 1.0f - std::abs(selected_lod -
                                                       float(channel))));
                    if (std::abs(int(colors[pixel * 4 + channel]) - expected) > 1)
                        std::cerr << "mip scenario=" << scenario << " mode=" << static_cast<u32>(mode) << " got=" << int(colors[pixel * 4 + channel]) << " expected=" << expected << '\n';
                    check(std::abs(int(colors[pixel * 4 + channel]) -
                                   expected) <= 1,
                          "TEX bias and fractional sampler clamps select the "
                          "intended mip colors");
                }
            }
        }
    }
    std::cout << "PASS: decoded TEX.LB/LBA bias, fractional sampler clamps and "
                 "sampler cache keys\n";
}

// A solid red 2x2 texture sampled with a custom border color: the filter
// weight that falls outside the texture must blend the guest border color.
void testTextureBorderEmulation() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    metal::Renderer border_renderer;
    auto* device = border_renderer.getDevice();
    auto* texture_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA8Unorm, 2, 2, false);
    texture_descriptor->setStorageMode(MTL::StorageModeShared);
    texture_descriptor->setUsage(MTL::TextureUsageShaderRead);
    NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
    const std::array<u8, 16> red = {255, 0, 0, 255, 255, 0, 0, 255,
                                    255, 0, 0, 255, 255, 0, 0, 255};
    texture->replaceRegion(MTL::Region::Make2D(0, 0, 2, 2), 0, red.data(), 8);
    const std::array<float, 4> border_color = {0.25f, 0.5f, 0.75f, 1.0f};

    for (const bool linear : {true, false}) {
        const auto filter = linear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest;
        const SamplerDescriptor sampler_descriptor{
            .min_filter = filter,
            .mag_filter = filter,
            .mip_filter = MTL::SamplerMipFilterNotMipmapped,
            .address_mode_s = MTL::SamplerAddressModeClampToBorderColor,
            .address_mode_t = MTL::SamplerAddressModeClampToBorderColor,
            .address_mode_r = MTL::SamplerAddressModeClampToBorderColor,
            .depth_compare_op =
                hydra::hw::tegra_x1::gpu::engines::CompareOp::Invalid,
            .border_color_u = std::bit_cast<simd::uint4>(border_color),
        };
        check(samplerNeedsBorderEmulation(sampler_descriptor),
              "A custom border color is emulated");
        DecompilerContext context{};
        context.type = ShaderType::Fragment;
        GuestShaderState guest{};
        guest.color_target_data_types[0] = MTL::DataTypeFloat;
        guest.border_samplers.emplace(7, sampler_descriptor);
        ir::Module module;
        ir::Builder builder(module);
        // Texel coordinates -0.5, 0, 0.5, 1 (in units of the 2 texel size)
        for (u32 axis = 0; axis < 2; ++axis) {
            const auto position = ir::Value::createAttrMemory(
                {RZ, SV_POSITION_BASE + axis * 4, true}, ir::ScalarType::F32);
            builder.opCopy(
                ir::Value::createRegister(static_cast<reg_t>(axis),
                                          ir::ScalarType::F32),
                builder.opAdd(builder.opMultiply(
                                  position, ir::Value::createConstantF(0.5f)),
                              ir::Value::createConstantF(-0.75f)));
        }
        builder.opCopy(ir::Value::createRegister(2, ir::ScalarType::F32),
                       ir::Value::createConstantF(0.0f));
        decoder::DecoderContext dc{context, nullptr, builder};
        decoder::emitTex(dc, std::bit_cast<decoder::InstTex>(
                                 0xc038000000000000ull | 4ull | (7ull << 16) |
                                 (2ull << 20) | (2ull << 28) | (15ull << 31) |
                                 (7ull << 36) |
                                 (static_cast<u64>(decoder::Lod::Lz) << 55)));
        for (u32 channel = 0; channel < 4; ++channel)
            builder.opCopy(
                ir::Value::createRegister(static_cast<reg_t>(channel),
                                          ir::ScalarType::F32),
                ir::Value::createRegister(static_cast<reg_t>(4 + channel),
                                          ir::ScalarType::F32));
        builder.opExit();
        analyzer::MemoryAnalyzer memory;
        memory.analyze(module);
        std::vector<u8> generated;
        ResourceMapping mapping;
        codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                               generated, mapping);
        emitter.emit(module);
        std::string source(generated.begin(), generated.end());
        check(source.find("hydra_bs7_pw") != std::string::npos,
              "The border samplers are declared");
        source += "\nvertex float4 border_vertex(uint id [[vertex_id]]) { "
                  "return float4(id==1?3.0:-1.0,id==2?3.0:-1.0,0,1); }\n";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        if (!library && error)
            std::cerr << error->localizedDescription()->utf8String() << '\n';
        check(library != nullptr, "Border emulated texture sample compiles");
        NS_STACK_SCOPED auto vertex =
            library->newFunction(toNsString("border_vertex"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto pipeline_descriptor =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pipeline_descriptor->setVertexFunction(vertex);
        pipeline_descriptor->setFragmentFunction(fragment);
        pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
            MTL::PixelFormatRGBA8Unorm);
        NS_STACK_SCOPED auto pipeline =
            device->newRenderPipelineState(pipeline_descriptor, &error);
        check(pipeline != nullptr, "Border pipeline compiles");
        auto* output_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA8Unorm, 4, 4, false);
        output_descriptor->setStorageMode(MTL::StorageModeShared);
        output_descriptor->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto output_texture =
            device->newTexture(output_descriptor);
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* attachment = pass->colorAttachments()->object(0);
        attachment->setTexture(output_texture);
        attachment->setLoadAction(MTL::LoadActionClear);
        attachment->setStoreAction(MTL::StoreActionStore);
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        auto* commands = queue->commandBuffer();
        auto* encoder = commands->renderCommandEncoder(pass);
        encoder->setRenderPipelineState(pipeline);
        const auto slot = mapping.textures.at(7);
        encoder->setFragmentTexture(texture, slot);
        // The shader samples with its own samplers; any bound one is unused
        NS_STACK_SCOPED auto bound_descriptor =
            MTL::SamplerDescriptor::alloc()->init();
        NS_STACK_SCOPED auto bound_sampler =
            device->newSamplerState(bound_descriptor);
        encoder->setFragmentSamplerState(bound_sampler, slot);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3));
        encoder->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        check(commands->status() == MTL::CommandBufferStatusCompleted,
              "Border render completes");
        std::array<u8, 64> colors{};
        output_texture->getBytes(colors.data(), 16,
                                 MTL::Region::Make2D(0, 0, 4, 4), 0);
        // Coordinates -0.5, 0, 0.5 and 1: the filter footprint of 0 and 1
        // straddles the edge; with nearest sampling [0, 1) is inside
        const std::array<float, 4> linear_coverage = {0.0f, 0.5f, 1.0f, 0.5f};
        const std::array<float, 4> nearest_coverage = {0.0f, 1.0f, 1.0f, 0.0f};
        const auto& axis_coverage = linear ? linear_coverage : nearest_coverage;
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                const float coverage = axis_coverage[x] * axis_coverage[y];
                const std::array<float, 4> inside = {1.0f, 0.0f, 0.0f, 1.0f};
                for (u32 channel = 0; channel < 4; ++channel) {
                    const float value = inside[channel] * coverage +
                                        border_color[channel] * (1.0f - coverage);
                    const int expected = static_cast<int>(value * 255.0f + 0.5f);
                    const int got = colors[(y * 4 + x) * 4 + channel];
                    if (std::abs(got - expected) > 3)
                        std::cerr << "border linear=" << linear << " x=" << x
                                  << " y=" << y << " channel=" << channel
                                  << " got=" << got
                                  << " expected=" << expected << '\n';
                    check(std::abs(got - expected) <= 3,
                          "The guest border color is blended by the filter "
                          "weight outside the texture");
                }
            }
        }
    }
    std::cout << "PASS: emulated custom sampler border colors\n";
}

void testTextureOffsetAndGather(MTL::Device* device) {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    using namespace shader_decomp;
    auto* input_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA8Unorm, 4, 4, false);
    input_descriptor->setStorageMode(MTL::StorageModeShared);
    input_descriptor->setUsage(MTL::TextureUsageShaderRead);
    NS_STACK_SCOPED auto input_texture = device->newTexture(input_descriptor);
    NS_STACK_SCOPED auto array_descriptor =
        MTL::TextureDescriptor::alloc()->init();
    array_descriptor->setTextureType(MTL::TextureType2DArray);
    array_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    array_descriptor->setWidth(4);
    array_descriptor->setHeight(4);
    array_descriptor->setArrayLength(2);
    array_descriptor->setStorageMode(MTL::StorageModeShared);
    array_descriptor->setUsage(MTL::TextureUsageShaderRead);
    NS_STACK_SCOPED auto array_texture = device->newTexture(array_descriptor);
    std::array<u8, 64> input_pixels{};
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            input_pixels[(y * 4 + x) * 4] = static_cast<u8>(1 + y * 10 + x);
            input_pixels[(y * 4 + x) * 4 + 3] = 255;
        }
    input_texture->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0,
                                 input_pixels.data(), 16);
    array_texture->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, 0,
                                 input_pixels.data(), 16, 64);
    auto layer_pixels = input_pixels;
    for (u32 pixel = 0; pixel < 16; ++pixel)
        layer_pixels[pixel * 4] += 100;
    array_texture->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, 1,
                                 layer_pixels.data(), 16, 64);
    NS_STACK_SCOPED auto sampler_descriptor =
        MTL::SamplerDescriptor::alloc()->init();
    sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
    sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
    sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    NS_STACK_SCOPED auto sampler = device->newSamplerState(sampler_descriptor);
    sampler_descriptor->setCompareFunction(MTL::CompareFunctionLess);
    NS_STACK_SCOPED auto depth_sampler =
        device->newSamplerState(sampler_descriptor);
    auto* depth_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatDepth32Float, 4, 4, false);
    depth_descriptor->setStorageMode(MTL::StorageModePrivate);
    depth_descriptor->setUsage(MTL::TextureUsageShaderRead |
                               MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto depth_texture = device->newTexture(depth_descriptor);
    {
        auto* clear_pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        clear_pass->depthAttachment()->setTexture(depth_texture);
        clear_pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        clear_pass->depthAttachment()->setStoreAction(MTL::StoreActionStore);
        clear_pass->depthAttachment()->setClearDepth(0.4);
        NS_STACK_SCOPED auto clear_queue = device->newCommandQueue();
        auto* clear_commands = clear_queue->commandBuffer();
        clear_commands->renderCommandEncoder(clear_pass)->endEncoding();
        clear_commands->commit();
        clear_commands->waitUntilCompleted();
        check(clear_commands->status() == MTL::CommandBufferStatusCompleted,
              "Gather depth fixture clears");
    }
    const std::array<std::array<u8, 4>, 8> expected{{{{23, 0, 0, 255}},
                                                     {{14, 0, 0, 255}},
                                                     {{13, 0, 0, 255}},
                                                     {{22, 23, 13, 12}},
                                                     {{13, 14, 4, 3}},
                                                     {{23, 22, 23, 2}},
                                                     {{122, 123, 113, 112}},
                                                     {{255, 255, 255, 255}}}};
    for (u32 scenario = 0; scenario < expected.size(); ++scenario) {
        DecompilerContext context{};
        context.type = ShaderType::Fragment;
        GuestShaderState guest{};
        guest.color_target_data_types[0] = MTL::DataTypeFloat;
        ir::Module module;
        ir::Builder builder(module);
        const auto undefined = ir::Value::createUndefined();
        auto flags = scenario >= 3 ? TextureSampleFlags::Gather
                                   : TextureSampleFlags::LodZero;
        auto coordinates = builder.opVectorConstruct(
            ir::ScalarType::F32, {ir::Value::createConstantF(0.5f),
                                  ir::Value::createConstantF(0.5f)});
        if (scenario == 2) {
            flags |= TextureSampleFlags::IntCoords;
            coordinates = builder.opVectorConstruct(
                ir::ScalarType::F32,
                {builder.opBitCast(ir::Value::createConstantU(1),
                                   ir::ScalarType::F32),
                 builder.opBitCast(ir::Value::createConstantU(2),
                                   ir::ScalarType::F32)});
        }
        auto offset = undefined;
        auto offsets = undefined;
        if (scenario == 1 || scenario == 2 || scenario == 4) {
            flags |= TextureSampleFlags::Offset;
            offset = builder.opVectorConstruct(
                ir::ScalarType::I32, {ir::Value::createConstantI(1),
                                      ir::Value::createConstantI(-1)});
        }
        if (scenario == 5) {
            flags |= TextureSampleFlags::Offsets;
            // Four distinct offsets with both negative X and negative Y.
            offsets = builder.opVectorConstruct(
                ir::ScalarType::U32, {ir::Value::createConstantU(0x003f0001),
                                      ir::Value::createConstantU(0x3f000100)});
        }
        const u32 binding = scenario == 0 ? makeConstantTextureIndex(0) : 7;
        auto value = undefined;
        if (scenario == 5 || scenario == 7) {
            if (scenario == 7)
                builder.opCopy(
                    ir::Value::createRegister(4, ir::ScalarType::F32),
                    ir::Value::createConstantF(0.25f));
            for (u32 coordinate = 0; coordinate < 2; ++coordinate)
                builder.opCopy(
                    ir::Value::createRegister(static_cast<reg_t>(coordinate),
                                              ir::ScalarType::F32),
                    ir::Value::createConstantF(0.5f));
            builder.opCopy(ir::Value::createRegister(2, ir::ScalarType::U32),
                           ir::Value::createConstantU(0x003f0001));
            builder.opCopy(ir::Value::createRegister(3, ir::ScalarType::U32),
                           ir::Value::createConstantU(0x3f000100));
            decoder::DecoderContext dc{context, nullptr, builder};
            decoder::emitTld4(dc,
                              std::bit_cast<decoder::InstTld4>(
                                  0xc838000000000000ull | 4ull | (7ull << 16) |
                                  (2ull << 20) | (2ull << 28) | (15ull << 31) |
                                  (7ull << 36) | (2ull << 54) |
                                  (u64(scenario == 7) << 50)));
            value = builder.opVectorConstruct(
                ir::ScalarType::F32,
                {ir::Value::createRegister(4, ir::ScalarType::F32),
                 ir::Value::createRegister(5, ir::ScalarType::F32),
                 ir::Value::createRegister(6, ir::ScalarType::F32),
                 ir::Value::createRegister(7, ir::ScalarType::F32)});
        } else if (scenario == 6) {
            builder.opCopy(ir::Value::createRegister(0, ir::ScalarType::U32),
                           ir::Value::createConstantU(1));
            builder.opCopy(ir::Value::createRegister(1, ir::ScalarType::F32),
                           ir::Value::createConstantF(0.5f));
            builder.opCopy(ir::Value::createRegister(2, ir::ScalarType::F32),
                           ir::Value::createConstantF(0.5f));
            decoder::DecoderContext dc{context, nullptr, builder};
            decoder::emitTld4(dc,
                              std::bit_cast<decoder::InstTld4>(
                                  0xc838000000000000ull | 4ull | (7ull << 16) |
                                  (3ull << 20) | (3ull << 28) | (15ull << 31) |
                                  (7ull << 36)));
            value = builder.opVectorConstruct(
                ir::ScalarType::F32,
                {ir::Value::createRegister(4, ir::ScalarType::F32),
                 ir::Value::createRegister(5, ir::ScalarType::F32),
                 ir::Value::createRegister(6, ir::ScalarType::F32),
                 ir::Value::createRegister(7, ir::ScalarType::F32)});
        } else {
            value = builder.opTextureSample(
                binding, shader_decomp::TextureType::_2D, flags, undefined,
                coordinates, undefined, undefined, undefined, undefined, offset,
                offsets);
        }
        for (u32 channel = 0; channel < 4; ++channel)
            builder.opCopy(
                ir::Value::createRegister(static_cast<reg_t>(channel),
                                          ir::ScalarType::F32),
                builder.opVectorExtract(value, static_cast<u8>(channel)));
        builder.opExit();
        analyzer::MemoryAnalyzer memory;
        memory.analyze(module);
        std::vector<u8> generated;
        ResourceMapping mapping;
        codegen::lang::msl::MslEmitter emitter(context, memory, guest,
                                               generated, mapping);
        emitter.emit(module);
        std::string source(generated.begin(), generated.end());
        source +=
            "\nvertex float4 texture_vertex(uint id [[vertex_id]]) { return "
            "float4(id==1?3.0:-1.0,id==2?3.0:-1.0,0,1); }\n";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        if (!library && error)
            std::cerr << error->localizedDescription()->utf8String() << '\n';
        check(library != nullptr, "Offset/gather shader compiles");
        NS_STACK_SCOPED auto vertex =
            library->newFunction(toNsString("texture_vertex"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto pipeline_descriptor =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pipeline_descriptor->setVertexFunction(vertex);
        pipeline_descriptor->setFragmentFunction(fragment);
        pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
            MTL::PixelFormatRGBA8Unorm);
        NS_STACK_SCOPED auto pipeline =
            device->newRenderPipelineState(pipeline_descriptor, &error);
        check(pipeline != nullptr, "Offset/gather pipeline compiles");
        auto* output_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA8Unorm, 4, 4, false);
        output_descriptor->setStorageMode(MTL::StorageModeShared);
        output_descriptor->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto output_texture =
            device->newTexture(output_descriptor);
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* attachment = pass->colorAttachments()->object(0);
        attachment->setTexture(output_texture);
        attachment->setLoadAction(MTL::LoadActionClear);
        attachment->setStoreAction(MTL::StoreActionStore);
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        auto* commands = queue->commandBuffer();
        auto* encoder = commands->renderCommandEncoder(pass);
        encoder->setRenderPipelineState(pipeline);
        const auto slot = mapping.textures.at(binding);
        encoder->setFragmentTexture(scenario == 7   ? depth_texture
                                    : scenario == 6 ? array_texture
                                                    : input_texture,
                                    slot);
        encoder->setFragmentSamplerState(
            scenario == 7 ? depth_sampler : sampler, slot);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3));
        encoder->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        check(commands->status() == MTL::CommandBufferStatusCompleted,
              "Offset/gather draw completes");
        std::array<u8, 64> actual{};
        output_texture->getBytes(actual.data(), 16,
                                 MTL::Region::Make2D(0, 0, 4, 4), 0);
        for (u32 pixel = 0; pixel < 16; ++pixel)
            for (u32 channel = 0; channel < 4; ++channel) {
                if (actual[pixel * 4 + channel] != expected[scenario][channel])
                    std::cerr << "texture scenario " << scenario << " channel "
                              << channel << " got "
                              << int(actual[pixel * 4 + channel])
                              << " expected "
                              << int(expected[scenario][channel]) << '\n';
                check(actual[pixel * 4 + channel] ==
                          expected[scenario][channel],
                      "Signed offsets and gather lanes read the expected "
                      "independent grid texels");
            }
    }
    std::cout << "PASS: LOD-zero, bindless bindings, integer/normalized "
                 "offsets and per-texel gathers\n";
}

int main() {
    try {
        unsetenv("HYDRA_DISABLE_SHADER_CACHE");
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        testClearColor(device);
        testBindlessPredicateTracking();
        testBindlessControlFlowTracking();
        testBitOperations(device);
        testIntegerShifts(device);
        testSharedAtomics(device);
        testLocalMemory(device);
        testRegisterPredicates(device);
        testIntegerConditions(device);
        testIndirectBranches(device);
        testCompareSelect(device, false);
        testCompareSelect(device, true);
        testSetRegisterResults(device);
        testVertexPositionDefaults(device);
        testFragmentDepth(device);
        testQuadDerivatives(device);
        testTextureLodSelection();
        testTextureBorderEmulation();
        testTextureOffsetAndGather(device);
        testGeometryLayerForwarding(device);
        testLea(device);
        testConditionCodes(device);
        runShaderCoverageTests(device);
        // Compile and execute the actual emitter's special float constants.
        // Check the bits on the GPU so signed infinity and NaN payloads survive.
        const std::array<u32, 4> special_bits{
            0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc12345u};
        std::string special_source =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void main_(device uint* output [[buffer(0)]]) {\n";
        for (usize i = 0; i < special_bits.size(); ++i)
            special_source += fmt::format(
                "output[{}] = as_type<uint>({});\n", i,
                ConstantEmitter::getConstantStr(std::bit_cast<f32>(special_bits[i])));
        special_source += "}\n";
        {
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
            options->setFastMathEnabled(false);
            NS_STACK_SCOPED auto library = device->newLibrary(
                toNsString(special_source), options, &error);
            check(library != nullptr, "Special float constants compile as MSL");
            NS_STACK_SCOPED auto function = library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
            check(pipeline != nullptr, "Special float pipeline is valid");
            NS_STACK_SCOPED auto output = device->newBuffer(
                sizeof(special_bits), MTL::ResourceStorageModeShared);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(std::memcmp(output->contents(), special_bits.data(), sizeof(special_bits)) == 0,
                  "GPU preserves special float constant bits");
        }
        const auto dir = std::filesystem::temp_directory_path() /
                         fmt::format("hydra-library-test-{}", getpid());
        std::filesystem::remove_all(dir);
        const std::string source = R"(#include <metal_stdlib>
using namespace metal;
kernel void main_(device uint* output [[buffer(0)]]) { output[0] = 123456; }
)";
        NS::Error* error = nullptr;
        {
            ShaderLibraryCache cache(device, dir);
            NS_STACK_SCOPED auto first = cache.get(source, &error);
            check(first != nullptr, "Cold shader compilation succeeds");
            cache.flush();
        }
        {
            ShaderLibraryCache cache(device, dir);
            cache.flush();
            NS_STACK_SCOPED auto library = cache.get(source, &error);
            check(library && cache.getHits() == 1,
                  "Second session uses prewarmed library");
            NS_STACK_SCOPED auto function =
                library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto pipeline =
                device->newComputePipelineState(function, &error);
            check(pipeline != nullptr,
                  "Prewarmed shader creates real GPU pipeline");
            NS_STACK_SCOPED auto output =
                device->newBuffer(sizeof(u32), MTL::ResourceStorageModeShared);
            *static_cast<u32*>(output->contents()) = 0;
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(*static_cast<u32*>(output->contents()) == 123456,
                  "Warm shader computes correct output");
            for (usize i = 0; i < 132; ++i) {
                NS_STACK_SCOPED auto variant = cache.get(
                    source + fmt::format("\n// variant {}", i), &error);
                check(variant != nullptr, "Variant compiles");
                if (i % 32 == 31)
                    cache.flush();
            }
            check(cache.getResidentCount() == ShaderLibraryCache::MAX_LIBRARIES,
                  "Resident library count stays bounded");
            cache.flush();
            NS_STACK_SCOPED auto bad =
                cache.get("invalid metal source", &error);
            check(bad == nullptr, "Invalid source remains a compile failure");
        }
        usize files = 0, bytes = 0;
        for (const auto& file : std::filesystem::directory_iterator(dir)) {
            if (file.path().extension() == ".metal") {
                ++files;
                bytes += file.file_size();
            }
        }
        check(files == 128 &&
                  bytes <= 64 * ShaderLibraryCache::MAX_SOURCE_BYTES,
              "Disk cache evicts old entries within count/byte limits");
        {
            ShaderLibraryCache cache(device, dir);
            cache.flush();
            NS_STACK_SCOPED auto newest =
                cache.get(source + "\n// variant 131", &error);
            check(newest && cache.getHits() == 1,
                  "Newest source survives disk eviction and prewarms");
        }
        std::filesystem::remove_all(dir);
        std::cout << "PASS: disk source persistence, warm-session GPU output, "
                     "resident limit, invalid shader\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

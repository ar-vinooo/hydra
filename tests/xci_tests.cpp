#include "common/common.hpp"

#include <fstream>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

#include "core/c_api.h"
#include "core/horizon/kernel/thread.hpp"
#include "core/horizon/kernel/shared_memory.hpp"
#include "core/horizon/services/hid/internal/applet_resource.hpp"
#include "core/horizon/services/hid/internal/shared_memory.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/engines/copy.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/system.hpp"
#ifdef HYDRA_HYPERVISOR_ENABLED
#include "core/hw/tegra_x1/cpu/hypervisor/physical_memory.hpp"
#endif
#include "core/horizon/filesystem/content_archive.hpp"
#include "core/horizon/filesystem/disk_file.hpp"
#include "core/horizon/filesystem/nca_file.hpp"
#include "core/horizon/filesystem/ncz_file.hpp"
#include "core/horizon/filesystem/romfs/romfs.hpp"
#include "core/horizon/loader/loader.hpp"

using namespace hydra;
using namespace hydra::horizon;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class CountingFile final : public filesystem::IFile {
  public:
    explicit CountingFile(const std::filesystem::path& path)
        : file(path.string()) {}
    u64 getSize() const override { return file.getSize(); }
    ztd::io::IStream* open(filesystem::FileOpenFlags flags) override {
        return new Stream(file.open(flags), reads);
    }
    usize reads = 0;

  private:
    class Stream final : public ztd::io::IStream {
      public:
        Stream(ztd::io::IStream* stream_, usize& reads_)
            : stream(stream_), reads(reads_) {}
        u64 getSeek() const noexcept override { return stream->getSeek(); }
        u64 getSize() const noexcept override { return stream->getSize(); }
        void seekTo(u64 offset) noexcept override { stream->seekTo(offset); }
        void seekBy(u64 offset) noexcept override { stream->seekBy(offset); }

      protected:
        void readRaw(std::span<u8> bytes) noexcept override {
            ++reads;
            stream->readToSpan(bytes);
        }

      private:
        std::unique_ptr<ztd::io::IStream> stream;
        usize& reads;
    };
    filesystem::DiskFile file;
};

class TestWaitThread final : public kernel::IThread {
  public:
    TestWaitThread() : IThread(nullptr, 0, "Wait test") {}
    uptr getTlsPtr() const override { return 0; }

  private:
    void run() override {}
};

struct TestMmuRange {
    u64 size;
    u32 id;
};

class TestMmu final : public hw::GenericMmu<TestMmu, TestMmuRange> {
  public:
    static inline usize size_queries = 0;
    static u64 implGetSize(const TestMmuRange& range) {
        ++size_queries;
        return range.size;
    }
    void mapImpl(uptr, TestMmuRange) {}
    void unmapImpl(uptr, TestMmuRange) {}
};

void checkMmuLookup() {
    TestMmu mmu;
    for (u32 i = 0; i < 4096; ++i)
        mmu.map(0x10000 + uptr(i) * 0x2000, {0x1000, i});
    TestMmu::size_queries = 0;
    uptr base = 0;
    for (u32 i = 0; i < 4096; ++i) {
        const auto range =
            mmu.findAddr(0x10000 + uptr(i) * 0x2000 + 0xfff, base);
        check(range && range->id == i, "Wrong GPU range lookup");
        check(base == 0x10000 + uptr(i) * 0x2000, "Wrong GPU range base");
    }
    check(TestMmu::size_queries == 4096,
          "GPU lookup still scans every mapping");
    // Repeated lookups of a recent mapping skip the map
    for (u32 i = 0; i < 16; ++i)
        check(mmu.findAddr(0x10000 + uptr(4095) * 0x2000 + i, base)->id ==
                  4095,
              "Cached GPU range lookup");
    check(TestMmu::size_queries == 4096, "Recent mappings are cached");
    check(!mmu.findAddr(0xffff, base), "Address before mapping accepted");
    check(!mmu.findAddr(0x11000, base), "Mapping boundary/gap accepted");
    mmu.map(0x10800, {0x1000, 9000});
    check(mmu.findAddr(0x10900, base)->id == 0,
          "Overlapping mapping precedence changed");
    check(mmu.findAddr(0x11000, base)->id == 9000,
          "Overlap extension unavailable");
    mmu.unmap(0x10800);
    check(!mmu.findAddr(0x11000, base),
          "Unmapped GPU range remains visible");
    // A remap replaces the cached mapping
    mmu.unmap(0x10000);
    mmu.map(0x10000, {0x1000, 7777});
    check(mmu.findAddr(0x10010, base)->id == 7777,
          "Cached lookup survives a remap");
    std::cout << "PASS: 4096 GPU lookups use one range check each; boundaries "
                 "and overlap precedence\n";

    // Lookup cost: the GPU thread resolves addresses constantly, mostly inside
    // a few mappings. The locked, cached lookup must stay close to a bare map.
    {
        constexpr u32 LOOKUPS = 4'000'000;
        std::map<uptr, TestMmuRange> bare;
        for (u32 i = 0; i < 4096; ++i)
            bare[0x10000 + uptr(i) * 0x2000] = {0x1000, i};
        const auto time = [&](auto&& lookup) {
            const auto begin = std::chrono::steady_clock::now();
            u64 sum = 0;
            for (u32 i = 0; i < LOOKUPS; ++i)
                sum += lookup(0x10000 + uptr((i >> 4) % 6) * 0x2000 + (i & 0xff));
            const auto end = std::chrono::steady_clock::now();
            check(sum != 0, "Lookups ran");
            return std::chrono::duration<double, std::nano>(end - begin).count() /
                   LOOKUPS;
        };
        uptr lookup_base = 0;
        const double bare_ns = time([&](uptr addr) {
            auto it = bare.upper_bound(addr);
            --it;
            return it->second.id + u64(1);
        });
        const double cached_ns = time([&](uptr addr) {
            return mmu.findAddr(addr, lookup_base)->id + u64(1);
        });
        std::cout << "GPU lookup: " << cached_ns << " ns (bare map " << bare_ns
                  << " ns)\n";
        check(cached_ns < bare_ns * 2.0 + 20.0,
              "The cached lookup is not slower than a bare map lookup");
    }

    // Mappings change on service threads while the GPU thread looks them up
    TestMmu shared;
    for (u32 i = 0; i < 64; ++i)
        shared.map(0x100000 + uptr(i) * 0x10000, {0x10000, i});
    std::atomic<bool> stop{false};
    std::thread mapper([&] {
        for (u32 round = 0; round < 20000; ++round) {
            const uptr addr = 0x2000000 + uptr(round % 512) * 0x1000;
            shared.map(addr, {0x1000, round});
            shared.unmap(addr);
        }
        stop = true;
    });
    u64 lookups = 0;
    while (!stop) {
        for (u32 i = 0; i < 64; ++i) {
            uptr lookup_base;
            const auto range =
                shared.findAddr(0x100000 + uptr(i) * 0x10000 + 0x123,
                                lookup_base);
            check(range && range->id == i,
                  "Lookup during concurrent remapping");
            lookups++;
        }
    }
    mapper.join();
    check(lookups != 0, "Lookups ran during remapping");
    std::cout << "PASS: GPU range lookups during concurrent remapping\n";
}

void checkThreadWaits() {
    TestWaitThread waiting;
    waiting.pause();
    check(!waiting.processMessages(1'000'000),
          "Unsignalled wait should time out");
    check(waiting.getState() == kernel::ThreadState::Running,
          "Timed out thread remains paused");
    for (int i = 0; i < 20; ++i) {
        waiting.pause();
        std::jthread signal([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waiting.supervisorResume();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waiting.resume();
        });
        check(waiting.processMessages(1'000'000'000), "Resume wait timed out");
        check(waiting.wasSignalled(), "Wait returned without a guest signal");
    }
    // CancelSynchronization ends a wait in progress...
    waiting.pause();
    waiting.beginSyncWait();
    waiting.cancelSync();
    check(waiting.processMessages(), "Cancel wait timed out");
    check(!waiting.wasSignalled(), "Cancellation was treated as a signal");
    waiting.endSyncWait();
    // ...and, sent before the thread waits, the next wait (no lost wakeup)
    waiting.cancelSync();
    waiting.pause();
    waiting.beginSyncWait();
    check(waiting.processMessages(), "Early cancel did not end the next wait");
    check(!waiting.wasSignalled(), "Early cancellation was a signal");
    waiting.endSyncWait();
    waiting.stop();
    waiting.processMessages();
    std::cout
        << "PASS: thread timeout, repeated signal waits and cancellation\n";
}

void checkShaderReadBounds() {
    namespace decomp = hw::tegra_x1::gpu::renderer::shader_decomp;
    std::array<u8, 7> bytes{};
    for (const u64 seek : {0ull, 7ull, 0x1000ull}) {
        ztd::io::MemoryStream stream(bytes);
        stream.seekTo(seek);
        decomp::ir::Module module;
        decomp::ir::Builder builder(module);
        const decomp::DecompilerContext context{
            .type = hw::tegra_x1::gpu::renderer::ShaderType::Compute};
        decomp::decoder::Decoder decoder({context, &stream, builder});
        decoder.decode();
        check(stream.getSeek() == seek,
              "Shader decoder consumed a partial or out-of-range instruction");
    }
    std::cout << "PASS: shader decoder rejects partial and out-of-range reads\n";
}

void checkGpuMappings(System* test_system, kernel::Process* test_process) {
    constexpr u64 page = hw::tegra_x1::cpu::GUEST_PAGE_SIZE;
    constexpr vaddr_t va = 0x7000000000;
    constexpr uptr gpu_va = 0x200000;
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> first(
        test_system->getCpu().allocateMemory(3 * page));
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> second(
        test_system->getCpu().wrapMemory(first->getPtr() + 2 * page, page));
    check(second->getPtr() != first->getPtr() + page,
          "Scattered GPU fixture must have separate backing");
    auto* cpu_mmu = test_process->getMmu();
    const kernel::MemoryState state{
        kernel::MemoryType::Normal_4_0_0,
        kernel::MemoryAttribute::None,
        kernel::MemoryPermission::ReadWrite};
    cpu_mmu->map(va, ztd::Range<uptr>::fromSize(first->getPtr(), page), state);
    cpu_mmu->map(va + page,
                 ztd::Range<uptr>::fromSize(second->getPtr(), page), state);
    hw::tegra_x1::gpu::GMmu gpu_mmu(cpu_mmu);
    gpu_mmu.mapBufferToAddressSpace(
        ztd::Range<vaddr_t>::fromSize(va, 2 * page), gpu_va);
    check(gpu_mmu.unmapAddr(gpu_va + page - 1) == first->getPtr() + page - 1 &&
              gpu_mmu.unmapAddr(gpu_va + page) == second->getPtr(),
          "GPU mapping treated scattered CPU pages as contiguous");
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> source(
        test_system->getCpu().allocateMemory(2 * page));
    cpu_mmu->map(va + 2 * page, source.get(), state);
    gpu_mmu.mapBufferToAddressSpace(
        ztd::Range<vaddr_t>::fromSize(va + 2 * page, 2 * page), 0x300000);
    auto* input = reinterpret_cast<u8*>(source->getPtr());
    for (u64 i = 0; i < 2 * page; ++i)
        input[i] = static_cast<u8>(i * 17 + 3);
    hw::tegra_x1::gpu::tls_crnt_gmmu = &gpu_mmu;
    hw::tegra_x1::gpu::engines::Copy copy(test_system->getGpu());
    copy.method(0x100, 0);
    copy.method(0x101, 0x300000);
    copy.method(0x102, 0);
    copy.method(0x103, static_cast<u32>(gpu_va));
    copy.method(0x104, 0);
    copy.method(0x105, 0);
    copy.method(0x106, static_cast<u32>(2 * page));
    copy.method(0x107, 0);
    copy.method(0xc0, 0x180); // Pitch/Pitch, one block, zero pitches/count.
    check(std::memcmp(reinterpret_cast<const void*>(first->getPtr()), input,
                      page) == 0 &&
              std::memcmp(reinterpret_cast<const void*>(second->getPtr()),
                          input + page, page) == 0,
          "Single-block DMA omitted bytes or crossed into the wrong physical "
          "allocation");
    const auto shader_bytes = gpu_mmu.readMappedBytes(gpu_va, 3 * page);
    check(shader_bytes.size() == 2 * page &&
              std::memcmp(shader_bytes.data(), input, 2 * page) == 0,
          "Shader snapshot crossed a mapping gap or lost scattered code");
    const auto shader_tail = gpu_mmu.readMappedBytes(gpu_va + 2 * page - 3, 8);
    check(shader_tail.size() == 3 &&
              std::memcmp(shader_tail.data(), input + 2 * page - 3, 3) == 0 &&
              gpu_mmu.readMappedBytes(gpu_va + 2 * page, 8).empty(),
          "Shader snapshot read beyond the final mapped bytes");
    std::memset(reinterpret_cast<void*>(first->getPtr()), 0xcc, page);
    copy.method(0x104, 32);
    copy.method(0x105, 64);
    copy.method(0x106, 16);
    copy.method(0x107, 2);
    copy.method(0xc0, 0x380); // Two pitched lines; preserve destination padding.
    const auto* output = reinterpret_cast<const u8*>(first->getPtr());
    check(std::memcmp(output, input, 16) == 0 &&
              std::memcmp(output + 64, input + 32, 16) == 0 &&
              output[16] == 0xcc && output[80] == 0xcc,
          "Pitched DMA copied row padding instead of line length");
    hw::tegra_x1::gpu::tls_crnt_gmmu = nullptr;
    cpu_mmu->unmap(ztd::Range<vaddr_t>::fromSize(va, 4 * page));
    std::cout << "PASS: GPU mapping resolves separate physical pages\n";
    std::cout << "PASS: single-block and padded-row DMA with scattered backing\n";
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-memory") {
            checkShaderReadBounds();
            CONFIG_INSTANCE.getCpuBackend() = CpuBackend::Dynarmic;
            auto* system = static_cast<System*>(hydraCreateSystem());
            check(system != nullptr, "Cannot create GPU memory test system");
            auto* process = system->getOs()
                                .getKernel()
                                .getProcessManager()
                                .createProcess("GPU memory test");
            checkGpuMappings(system, process);
            {
                namespace hid = horizon::services::hid;
                hid::internal::AppletResource resource(*system);
                auto* memory = reinterpret_cast<hid::internal::SharedMemory*>(
                    resource.getSharedMemory()->getPtr());
                std::array<u64, 4> header{};
                std::memcpy(
                    header.data(),
                    &memory->npad.entries[0].internal_state.full_key_lifo,
                    sizeof(header));
                check(header[1] == 17 && header[2] == 1 && header[3] == 1,
                      "HID Npad ring header lacks initialized capacity");
                std::memcpy(header.data(), &memory->touch_screen.lifo,
                            sizeof(header));
                check(header[1] == 17 && header[3] == 1,
                      "HID touch ring header or neutral sample is invalid");
                const auto keyboard = memory->keyboard.lifo.getCurrentStorage();
                check(keyboard.has_value() && (*keyboard)->modifiers == 0 &&
                          std::all_of(std::begin((*keyboard)->keys),
                                      std::end((*keyboard)->keys),
                                      [](u64 keys) { return keys == 0; }),
                      "Guest keyboard has no neutral sample");
                check(memory->mouse.lifo.getCurrentStorage().has_value() &&
                          memory->debug_pad.lifo.getCurrentStorage().has_value(),
                      "Auxiliary input devices have empty buffers");
                auto& npad_state = memory->npad.entries[0].internal_state;
                const auto buttons = [](auto& lifo) {
                    return lifo.getCurrentStorage().value()->buttons;
                };
                hid::internal::Npad pad(npad_state);
                pad.setup(hid::NpadStyleSet::FullKey);
                input::NpadState pressed{};
                pressed.buttons = hid::NpadButtons::Up;
                pad.update(pressed);
                check(buttons(npad_state.full_key_lifo) == hid::NpadButtons::Up &&
                          buttons(npad_state.system_ext_lifo) == hid::NpadButtons::Up,
                      "Connected input missing from style or generic buffer");
                check(buttons(npad_state.handheld_lifo) == hid::NpadButtons::None,
                      "Inactive style received connected controller input");
                pad.update({});
                check(buttons(npad_state.full_key_lifo) == hid::NpadButtons::None &&
                          buttons(npad_state.system_ext_lifo) == hid::NpadButtons::None,
                      "Released key remains in guest input buffers");
                pad.setup(hid::NpadStyleSet::None);
                pad.update(pressed);
                check(buttons(npad_state.full_key_lifo) == hid::NpadButtons::None,
                      "Disconnected controller retains pressed input");
                std::cout << "PASS: HID ring initialization, inactive styles, "
                             "key release and disconnect\n";
            }
            hydraSystemDestroy(system);
            return 0;
        }
        check(argc >= 2, "Usage: hydra-xci-tests FIXTURE_DIRECTORY [REAL_XCI]");
        checkMmuLookup();
        checkThreadWaits();
        const std::filesystem::path dir(argv[1]);
        const auto keys = filesystem::NcaFile::loadKeys(dir / "prod.keys");
        auto source = std::make_shared<filesystem::DiskFile>(
            (dir / "encrypted.nca").string());
        bool plaintext;
        const auto header = filesystem::NcaFile::readHeader(
            *source, 0, source->getSize(), keys, plaintext);
        check(!plaintext, "Encrypted header not detected");
        filesystem::NcaFile decoded(source, 0, source->getSize(), header,
                                    plaintext, keys);
        filesystem::DiskFile expected((dir / "expected.nca").string());
        std::unique_ptr<ztd::io::IStream> actual_stream(
            decoded.open(filesystem::FileOpenFlags::Read));
        std::unique_ptr<ztd::io::IStream> expected_stream(
            expected.open(filesystem::FileOpenFlags::Read));
        std::vector<u8> reference(expected.getSize());
        expected_stream->readToSpan(std::span(reference));
        std::vector<u8> full(decoded.getSize());
        actual_stream->readToSpan(std::span(full));
        check(full == reference,
              "Full AES-XTS/CTR decryption differs from independent reference");
        // Unaligned random reads, crossing header and crypto-section
        // boundaries.
        for (u64 i = 0; i < 500; ++i) {
            const auto offset = (i * 1543) % decoded.getSize();
            const auto count =
                std::min<u64>(1 + (i * 43) % 700, decoded.getSize() - offset);
            std::vector<u8> bytes(count);
            actual_stream->seekTo(offset);
            actual_stream->readToSpan(std::span(bytes));
            check(std::equal(bytes.begin(), bytes.end(),
                             reference.begin() +
                                 static_cast<std::ptrdiff_t>(offset)),
                  "Unaligned AES read differs from reference");
        }
        auto large_source =
            std::make_shared<CountingFile>(dir / "large-ctr.nca");
        auto large_header = filesystem::NcaFile::readHeader(
            *large_source, 0, large_source->getSize(), keys, plaintext);
        filesystem::NcaFile large_decoded(large_source, 0,
                                          large_source->getSize(), large_header,
                                          plaintext, keys);
        std::unique_ptr<ztd::io::IStream> cached(
            large_decoded.open(filesystem::FileOpenFlags::Read));
        filesystem::DiskFile large_expected(
            (dir / "large-ctr-expected.nca").string());
        std::unique_ptr<ztd::io::IStream> large_reference_stream(
            large_expected.open(filesystem::FileOpenFlags::Read));
        std::vector<u8> large_reference(large_expected.getSize());
        large_reference_stream->readToSpan(std::span(large_reference));
        large_source->reads = 0;
        for (u64 i = 0; i < 1000; ++i) {
            cached->seekTo(0xc00 + i);
            check(cached->read<u8>() == large_reference[0xc00 + i],
                  "Cached byte differs from independent AES reference");
        }
        check(large_source->reads == 1,
              "Small CTR reads did not share one decrypted cache window");
        for (u64 offset :
             {0xfff0ull, 0x1fff0ull, 0x31ff0ull, 0x32000ull, 0xc00ull}) {
            for (u64 requested : {128ull, 0x10020ull}) {
                const auto count =
                    std::min<u64>(requested, large_reference.size() - offset);
                std::vector<u8> bytes(count);
                cached->seekTo(offset);
                cached->readToSpan(std::span(bytes));
                check(
                    std::equal(bytes.begin(), bytes.end(),
                               large_reference.begin() +
                                   static_cast<std::ptrdiff_t>(offset)),
                    "CTR cache window/section crossing differs from reference");
            }
        }
        std::cout << "PASS: 1000 small CTR reads use one source read; "
                     "cache/section crossings match independent AES\n";
        auto wrong_keys = keys;
        wrong_keys["header_key"][0] ^= 0xff;
        bool rejected = false;
        try {
            (void)filesystem::NcaFile::readHeader(*source, 0, source->getSize(),
                                                  wrong_keys, plaintext);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Incorrect header key was accepted");
        rejected = false;
        try {
            (void)filesystem::NcaFile::readHeader(*source, 0, source->getSize(),
                                                  {}, plaintext);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Missing keys were accepted");

        for (const auto* filename :
             {"base.xci", "base-with-update.xci", "readonly.xci"}) {
            auto result =
                loader::ILoader::createFromPath((dir / filename).string());
            check(result.has_value(), "Valid synthetic XCI was rejected");
            std::unique_ptr<loader::ILoader> game(*result);
            check(game->getTitleId() == 0x0100000000001000,
                  "Wrong base title selected");
            check(game->hasExeFs() && game->hasRomFs(),
                  "Missing XCI filesystems");
        }
        for (const auto* filename :
             {"update-only.xci", "multiple-base.xci", "truncated.xci",
              "bad-magic.xci", "bad-offset.xci", "bad-name.xci",
              "bad-section.xci", "bad-exefs.xci"}) {
            check(!loader::ILoader::createFromPath((dir / filename).string()),
                  "Invalid XCI was accepted");
            check(!loader::ILoader::getLastError().empty(),
                  "XCI error lacks explanation");
        }
        for (const auto* filename : {"base.nsp", "BASE.NSP", "ticket.nsp"}) {
            auto result =
                loader::ILoader::createFromPath((dir / filename).string());
            check(result.has_value(), "Valid NSP was rejected");
            std::unique_ptr<loader::ILoader> game(*result);
            check(game->getTitleId() == 0x0100000000001000 &&
                      game->hasExeFs() && game->hasRomFs(),
                  "Wrong NSP program or filesystems");
        }
        for (const auto* filename :
             {"update-only.nsp", "multiple-base.nsp", "truncated.nsp",
              "bad-offset.nsp", "no-program.nsp", "bad-ticket.nsp",
              "conflicting-ticket.nsp"}) {
            check(!loader::ILoader::createFromPath((dir / filename).string()),
                  "Invalid NSP was accepted");
            check(!loader::ILoader::getLastError().empty(),
                  "NSP failure lacks explanation");
        }
        const auto game32_result =
            loader::ILoader::createFromPath((dir / "32-bit.nsp").string());
        check(game32_result.has_value(),
              "32-bit title must remain visible in the library");
        std::unique_ptr<loader::ILoader> game32(*game32_result);
        check(!game32->getUnsupportedReason().empty() && game32->hasRomFs(),
              "32-bit compatibility reason or metadata missing");
        auto rights_source = std::make_shared<filesystem::DiskFile>(
            (dir / "rights-encrypted.nca").string());
        const auto rights_header = filesystem::NcaFile::readHeader(
            *rights_source, 0, rights_source->getSize(), keys, plaintext);
        filesystem::NcaFile rights_file(rights_source, 0,
                                        rights_source->getSize(), rights_header,
                                        plaintext, keys);
        std::unique_ptr<ztd::io::IStream> rights_stream(
            rights_file.open(filesystem::FileOpenFlags::Read));
        std::vector<u8> rights_actual(rights_file.getSize());
        rights_stream->readToSpan(std::span(rights_actual));
        filesystem::DiskFile rights_expected(
            (dir / "rights-expected.nca").string());
        std::unique_ptr<ztd::io::IStream> rights_expected_stream(
            rights_expected.open(filesystem::FileOpenFlags::Read));
        std::vector<u8> rights_reference(rights_expected.getSize());
        rights_expected_stream->readToSpan(std::span(rights_reference));
        check(rights_actual == rights_reference,
              "Ticket title-key decryption differs from independent AES "
              "reference");
        auto missing_title_key = keys;
        std::erase_if(missing_title_key, [](const auto& entry) {
            return entry.first.starts_with("encrypted_titlekey_");
        });
        bool missing_ticket_rejected = false;
        try {
            filesystem::NcaFile missing(rights_source, 0,
                                        rights_source->getSize(), rights_header,
                                        plaintext, missing_title_key);
        } catch (const std::exception&) {
            missing_ticket_rejected = true;
        }
        check(missing_ticket_rejected, "Missing matching ticket was accepted");
        std::cout << "PASS: NSP detection, malformed packages/tickets, "
                     "title-key AES reference\n";

        // NCZ sections are stored decrypted: only the header key is needed.
        filesystem::DiskFile ncz_expected(
            (dir / "ncz-expected.nca").string());
        std::unique_ptr<ztd::io::IStream> ncz_expected_stream(
            ncz_expected.open(filesystem::FileOpenFlags::Read));
        std::vector<u8> ncz_reference(ncz_expected.getSize());
        ncz_expected_stream->readToSpan(std::span(ncz_reference));
        const filesystem::NcaFile::Keys header_only{
            {"header_key", keys.at("header_key")}};
        const auto ncz_cache_dir =
            std::filesystem::temp_directory_path() /
            fmt::format("hydra-ncz-test-{}", ::getpid());
        std::filesystem::remove_all(ncz_cache_dir);
        for (const auto* filename : {"ncz-block.ncz", "ncz-solid.ncz"}) {
            const bool block = std::string_view(filename) == "ncz-block.ncz";
            auto ncz_source = std::make_shared<filesystem::DiskFile>(
                (dir / filename).string());
            const auto ncz_header = filesystem::NcaFile::readHeader(
                *ncz_source, 0, ncz_source->getSize(), header_only, plaintext);
            const auto cache = ncz_cache_dir / filename;
            // The second solid pass must reuse the completed cache.
            for (int pass = 0; pass < 2; ++pass) {
                auto ncz = std::make_shared<filesystem::NczFile>(
                    ncz_source, 0, ncz_source->getSize(), ncz_reference.size(),
                    cache);
                check(ncz->isBlockCompressed() == block,
                      "NCZ compression mode misdetected");
                filesystem::NcaFile ncz_nca(ncz, 0, ncz->getSize(), ncz_header,
                                            plaintext, header_only, true);
                std::unique_ptr<ztd::io::IStream> stream(
                    ncz_nca.open(filesystem::FileOpenFlags::Read));
                // Random reads first, so the solid cache grows on demand.
                for (u64 i = 0; i < 300; ++i) {
                    const auto offset = (i * 104729) % ncz_reference.size();
                    const auto count = std::min<u64>(
                        1 + (i * 4099) % 0x9000, ncz_reference.size() - offset);
                    std::vector<u8> bytes(count);
                    stream->seekTo(offset);
                    stream->readToSpan(std::span(bytes));
                    check(std::equal(bytes.begin(), bytes.end(),
                                     ncz_reference.begin() +
                                         static_cast<std::ptrdiff_t>(offset)),
                          "Random NCZ read differs from reference");
                }
                std::vector<u8> ncz_full(ncz_nca.getSize());
                stream->seekTo(0);
                stream->readToSpan(std::span(ncz_full));
                check(ncz_full == ncz_reference,
                      "Decompressed NCZ differs from reference NCA");
            }
            check(std::filesystem::exists(cache) != block,
                  "Solid NCZ cache missing (or block NCZ was cached)");
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(ncz_cache_dir))
            check(entry.path().extension() != ".part",
                  "Partial NCZ cache left behind");
        std::filesystem::remove_all(ncz_cache_dir);
        for (const auto* filename : {"base.nsz", "BASE.NSZ", "base.xcz"}) {
            auto result =
                loader::ILoader::createFromPath((dir / filename).string());
            check(result.has_value(), "Valid NSZ/XCZ was rejected");
            std::unique_ptr<loader::ILoader> game(*result);
            check(game->getTitleId() == 0x0100000000001000 &&
                      game->hasExeFs() && game->hasRomFs(),
                  "Wrong NSZ/XCZ program or filesystems");
        }
        for (const auto* filename : {"bad-magic.nsz", "truncated.nsz"}) {
            check(!loader::ILoader::createFromPath((dir / filename).string()),
                  "Invalid NSZ was accepted");
            check(!loader::ILoader::getLastError().empty(),
                  "NSZ failure lacks explanation");
        }
        std::cout << "PASS: block/solid NCZ match reference NCA, solid cache "
                     "reuse, NSZ/XCZ loading and malformed NSZ\n";
        check(!loader::ILoader::createFromPath(""), "Empty path accepted");
        check(!loader::ILoader::createFromPath((dir / "not-a-game").string()),
              "Extensionless file accepted");
        std::cout
            << "PASS: independent AES vectors, 500 random reads, base/update "
               "selection, malformed XCI and wrong/missing keys\n";
        if (argc >= 3) {
            auto result = loader::ILoader::createFromPath(argv[2]);
            if (!result)
                throw std::runtime_error(
                    std::string(loader::ILoader::getLastError()));
            std::unique_ptr<loader::ILoader> game(*result);
            if (argc >= 4 && std::string_view(argv[3]) == "--extract-exefs")
                game->extractExeFs((dir / "real-exefs").string());
            std::unique_ptr<services::ns::ApplicationControlProperty> nacp(
                game->loadNacp());
            check(nacp != nullptr, "Real package control metadata missing");
            u32 width = 0;
            u32 height = 0;
            auto icon = game->loadIcon(width, height);
            check(icon != nullptr && width != 0 && height != 0,
                  "Real package icon missing");
            std::free(icon);
            std::cout << "PASS: real package title " << std::hex
                      << game->getTitleId() << ", " << nacp->titles[0].name
                      << ", icon " << std::dec << width << 'x' << height
                      << '\n';
            if (!game->getUnsupportedReason().empty())
                std::cout << "Compatibility: " << game->getUnsupportedReason() << '\n';
        }
        if (argc >= 4 &&
            std::string_view(argv[3]) == "--check-firmware-lifetime") {
#ifdef HYDRA_HYPERVISOR_ENABLED
            using namespace hw::tegra_x1::cpu::hypervisor;
            uint32_t ipa_bits = 0;
            check(hv_vm_config_get_max_ipa_size(&ipa_bits) == HV_SUCCESS,
                  "Cannot query IPA range");
            constexpr u64 heap_size = 0x7fe00000;
            const auto heap = allocateVmMemory(heap_size);
            const auto physical =
                PhysicalMemory::instance().add(heap, heap_size);
            check(
                physical < (1ull << ipa_bits) &&
                    heap_size <= (1ull << ipa_bits) - physical,
                "Large heap physical allocation exceeds Hypervisor IPA range");
            check(PhysicalMemory::instance().fromHost(heap + 0x4000, 0x4000) ==
                      physical + 0x4000,
                  "Host to physical translation failed");
            check(PhysicalMemory::instance().toHost(physical + heap_size - 1) ==
                      heap + heap_size - 1,
                  "Physical to host translation failed");
            PhysicalMemory::instance().remove(physical);
            check(PhysicalMemory::instance().toHost(physical) == 0,
                  "Released mapping remains visible");
            auto* heap_bytes = reinterpret_cast<volatile u8*>(heap);
            heap_bytes[0] = 0x42;
            heap_bytes[heap_size - 1] = 0x24;
            check(heap_bytes[0] == 0x42 && heap_bytes[heap_size - 1] == 0x24,
                  "Heap backing inaccessible");
            freeVmMemory(heap, heap_size);
            std::cout
                << "PASS: 2 GB heap fits supported Hypervisor IPA range\n";
#endif
            CONFIG_INSTANCE.getCpuBackend() = CpuBackend::Dynarmic;
            auto* test_system = static_cast<System*>(hydraCreateSystem());
            check(test_system != nullptr,
                  "Cannot create test system with selected firmware");
            {
                namespace hid = services::hid;
                const auto saved_profiles = CONFIG_INSTANCE.getInputProfiles();
                CONFIG_INSTANCE.getInputProfiles() =
                    Config::getDefaultInputProfiles();
                hid::internal::AppletResource input_resource(*test_system);
                input_resource.clearSupportedNpads();
                input_resource.setNpadSupported(hid::internal::NpadIndex::No1,
                                                true);
                input_resource.activateNpads(hid::NpadRevision::Revision0);
                auto* input_memory =
                    reinterpret_cast<hid::internal::SharedMemory*>(
                        input_resource.getSharedMemory()->getPtr());
                check(input_memory->npad.entries[0].internal_state.style_set ==
                          hid::NpadStyleSet::FullKey,
                      "Supported player 1 controller missing");
                for (usize i = 1; i < 10; ++i)
                    check(input_memory->npad.entries[i]
                                  .internal_state.style_set ==
                              hid::NpadStyleSet::None,
                          "Unsupported controller advertised as connected");
                input_resource.setNpadSupported(
                    hid::internal::NpadIndex::Handheld, true);
                input_resource.setupNpads();
                check(input_memory->npad.entries[8].internal_state.style_set ==
                          hid::NpadStyleSet::None,
                      "Controller without an input profile advertised as "
                      "connected");
                CONFIG_INSTANCE.getInputProfiles()[8] = "Default";
                input_resource.setupNpads();
                check(input_memory->npad.entries[8].internal_state.style_set ==
                          hid::NpadStyleSet::Handheld,
                      "Configured handheld controller missing");
                input_resource.setSupportedStyleSet(
                    hid::NpadStyleSet::Handheld);
                input_resource.setupNpads();
                check(input_memory->npad.entries[0].internal_state.style_set ==
                          hid::NpadStyleSet::None,
                      "Unsupported controller style advertised as connected");
                std::cout << "PASS: HID exposes only supported controller IDs "
                             "and styles\n";
                CONFIG_INSTANCE.getInputProfiles() = saved_profiles;
            }
            auto& test_gpu = test_system->getGpu();
            std::vector<hydra::Handle> maps;
            for (u32 i = 0; i < 4096; ++i) {
                auto map = test_gpu.createMap(0x1000 + i);
                check(map.has_value(),
                      "GPU map table exhausted below 4096 maps");
                maps.push_back(*map);
            }
            auto first_map = *test_gpu.getMap(maps.front());
            check(first_map->size == 0x1000,
                  "GPU map invalidated by table growth");
            for (auto handle : maps)
                check(test_gpu.freeMap(handle), "Cannot free GPU map");
            check(!test_gpu.getMap(maps.front()),
                  "Freed GPU handle remains valid");
            auto reused = test_gpu.createMap(0x2000);
            check(reused.has_value(), "GPU handles not reusable");
            check(test_gpu.freeMap(*reused), "Cannot free reused GPU map");
            check(!test_gpu.freeMap(*reused), "Double GPU free accepted");
            std::cout
                << "PASS: 4096 GPU maps, stable addresses and handle reuse\n";
            auto& test_kernel = test_system->getOs().getKernel();
            auto* test_process = test_kernel.getProcessManager().createProcess(
                "Memory accounting test");
            test_process->setSystemResourceSize(0);
            checkGpuMappings(test_system, test_process);
            TestWaitThread address_waiter;
            u32 address_value = 1;
            check(test_kernel.waitForAddress(
                      &address_waiter, reinterpret_cast<uptr>(&address_value),
                      kernel::ArbitrationType::WaitIfEqual, 1,
                      0) == MAKE_RESULT(Svc, kernel::Error::TimedOut),
                  "Address arbitration timeout was lost");
            test_kernel.signalToAddress(reinterpret_cast<uptr>(&address_value),
                                        kernel::SignalType::Signal, 0, 1);
            address_waiter.pause();
            check(!address_waiter.processMessages(0),
                  "Timed out address waiter was signalled again");
            address_waiter.stop();
            address_waiter.processMessages();
            address_value = 5;
            check(test_kernel.signalToAddress(
                      reinterpret_cast<uptr>(&address_value),
                      kernel::SignalType::SignalAndIncrementIfEqual, 4,
                      1) == MAKE_RESULT(Svc, kernel::Error::InvalidState) &&
                      address_value == 5,
                  "Signal changed a mismatched address value");
            check(test_kernel.signalToAddress(
                      reinterpret_cast<uptr>(&address_value),
                      kernel::SignalType::SignalAndIncrementIfEqual, 5,
                      1) == RESULT_SUCCESS &&
                      address_value == 6,
                  "Signal failed to increment matching address value");
            check(test_kernel.signalToAddress(
                      reinterpret_cast<uptr>(&address_value),
                      kernel::SignalType::
                          SignalAndModifyBasedOnWaitingThreadCountIfEqual,
                      6, 1) == RESULT_SUCCESS &&
                      address_value == 7,
                  "Signal failed to modify address with no waiters");
            TestWaitThread decrement_waiter;
            check(test_kernel.waitForAddress(
                      &decrement_waiter, reinterpret_cast<uptr>(&address_value),
                      kernel::ArbitrationType::DecrementAndWaitIfLessThan, 7,
                      0) == MAKE_RESULT(Svc, kernel::Error::InvalidState) &&
                      address_value == 7,
                  "Conditional wait decremented an ineligible value");
            address_value = static_cast<u32>(-1);
            check(test_kernel.waitForAddress(
                      &decrement_waiter, reinterpret_cast<uptr>(&address_value),
                      kernel::ArbitrationType::DecrementAndWaitIfLessThan, 0,
                      0) == MAKE_RESULT(Svc, kernel::Error::TimedOut) &&
                      address_value == static_cast<u32>(-2),
                  "Conditional wait ignored signed comparison/decrement");
            decrement_waiter.stop();
            decrement_waiter.processMessages();
            std::cout << "PASS: conditional address signals, signed waits and "
                         "timeout cleanup\n";
            test_process->createStackMemory(0x4000);
            test_process->resizeHeap(0x800000);
            check(test_process->getUsedUserMemorySize() == 0x804000,
                  "Incorrect stack/heap accounting");
            u64 total = 0, non_system = 0, used = 0, used_non_system = 0;
            test_kernel.getInfo(test_process, kernel::InfoType::TotalMemorySize,
                                test_process, 0, total);
            test_kernel.getInfo(test_process,
                                kernel::InfoType::TotalNonSystemMemorySize,
                                test_process, 0, non_system);
            test_kernel.getInfo(test_process, kernel::InfoType::UsedMemorySize,
                                test_process, 0, used);
            test_kernel.getInfo(test_process,
                                kernel::InfoType::UsedNonSystemMemorySize,
                                test_process, 0, used_non_system);
            check(total == non_system && used == used_non_system &&
                      used == 0x804000,
                  "Memory totals are inconsistent with zero system resources");
            test_process->resizeHeap(0x1000000);
            check(test_process->getUsedUserMemorySize() == 0x1004000,
                  "Heap resize not reflected in memory usage");
            test_process->setSystemResourceSize(0x100000);
            test_kernel.getInfo(test_process,
                                kernel::InfoType::TotalNonSystemMemorySize,
                                test_process, 0, non_system);
            check(total - non_system == 0x100000,
                  "System resource budget not deducted");
            hydraSystemDestroy(test_system);
            std::cout << "PASS: consistent total/used memory accounting and "
                         "heap resize\n";
            bool tested = false;
            for (const auto& entry : std::filesystem::directory_iterator(
                     CONFIG_INSTANCE.getFirmwarePath())) {
                if (entry.path().extension() != ".nca")
                    continue;
                auto firmware_source = std::make_unique<filesystem::DiskFile>(
                    entry.path().string());
                filesystem::IFile* font = nullptr;
                {
                    filesystem::ContentArchive archive(firmware_source.get());
                    if (archive.getTitleId() != 0x0100000000000811 ||
                        archive.getContentType() !=
                            filesystem::ContentArchiveContentType::Data)
                        continue;
                    filesystem::IFile* data = nullptr;
                    check(archive.getFile("data", data) ==
                              filesystem::FsResult::Success,
                          "Missing font data");
                    filesystem::romfs::RomFS romfs(data);
                    check(romfs.getFile("nintendo_udsg-r_std_003.bfttf",
                                        font) == filesystem::FsResult::Success,
                          "Missing standard font");
                }
                // Reproduce SharedFontManager: open the returned font only
                // after the local ContentArchive and RomFS have been destroyed.
                std::unique_ptr<ztd::io::IStream> stream(
                    font->open(filesystem::FileOpenFlags::Read));
                check((stream->read<u32>() ^ 0x06186249) == 0x18029a7f,
                      "Invalid font after archive destruction");
                tested = true;
                break;
            }
            check(tested, "No standard font archive found");
            std::cout << "PASS: encrypted firmware font remains readable after "
                         "archive destruction\n";
            const auto saved_firmware = CONFIG_INSTANCE.getFirmwarePath();
            const auto incomplete_firmware = dir / "incomplete-firmware";
            std::filesystem::create_directories(incomplete_firmware);
            CONFIG_INSTANCE.getFirmwarePath() = incomplete_firmware.string();
            check(hydraCreateFilesystem() == nullptr,
                  "Incomplete firmware accepted");
            check(hydraFilesystemGetLastError().size != 0,
                  "Incomplete firmware lacks a recoverable error");
            CONFIG_INSTANCE.getFirmwarePath() = saved_firmware;
            std::cout << "PASS: incomplete firmware returns an error without "
                         "terminating\n";
        }
        if (argc >= 4 && std::string_view(argv[3]) == "--check-startup-error") {
            auto filesystem = hydraCreateFilesystem();
            check(filesystem == nullptr,
                  "Expected incompatible firmware to return no filesystem");
            check(hydraFilesystemGetLastError().size != 0,
                  "Settings firmware error lacks a message");
            check(hydraCreateTimeZoneManager(nullptr) == nullptr,
                  "Null filesystem accepted");
            std::cout << "PASS: settings firmware failure returned an error "
                         "without crashing\n";
            // Test error propagation without requiring hypervisor entitlements.
            CONFIG_INSTANCE.getCpuBackend() = CpuBackend::Dynarmic;
            auto system = hydraCreateSystem();
            if (system != nullptr) {
                hydraSystemDestroy(system);
                throw std::runtime_error(
                    "Expected unsupported firmware to return an error");
            }
            const auto error = hydraSystemGetLastError();
            check(error.size != 0,
                  "System initialization error lacks a message");
            std::cout << "PASS: firmware startup failure returned an error "
                         "without crashing\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

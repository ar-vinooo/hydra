#include "core/horizon/kernel/hipc/client_session.hpp"
#include "core/horizon/kernel/hipc/cmif.hpp"
#include "core/horizon/kernel/host_thread.hpp"
#include "core/horizon/kernel/event.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/horizon/services/server.hpp"
#include "core/horizon/services/audio/audio_out.hpp"
#include "core/horizon/services/audio/audio_renderer.hpp"
#include "core/horizon/services/mmnv/request.hpp"
#include "core/horizon/services/aocsrv/add_on_content_manager.hpp"
#include "core/video/submit_trace.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_nvdec.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_vic.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_ctrl.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_gpu.hpp"
#include "core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.hpp"
#include <filesystem>
#include <fstream>
#include "core/horizon/ui/handler_base.hpp"
#include "core/system.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
using namespace hydra;
using namespace hydra::horizon;
class Ui final : public ui::IHandler {
  public:
    void showMessageDialog(ui::MessageDialogType, const std::string&,
                           const std::string&) override {}
    applets::software_keyboard::SoftwareKeyboardResult
    showSoftwareKeyboard(const std::string&, const std::string&,
                         const std::string&, std::string&) override {
        return {};
    }
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
class TrackingService final : public services::IService {
  public:
    TrackingService(int& destroyed_, bool root_)
        : destroyed(destroyed_), root(root_) {}
    ~TrackingService() override { ++destroyed; }

  private:
    int& destroyed;
    bool root;
    kernel::result_t requestImpl(services::RequestContext& context,
                                 u32) override {
        if (root)
            addService(context, new TrackingService(destroyed, false));
        return RESULT_SUCCESS;
    }
};
void testDomainShutdown(System& system, kernel::Process* process) {
    namespace hipc = kernel::hipc;
    int destroyed = 0;
    services::Server server(system);
    auto* service = new TrackingService(destroyed, true);
    auto* server_session = new hipc::ServerSession();
    auto* client_session = new hipc::ClientSession();
    new hipc::Session(server_session, client_session);
    process->addHandleNoRetain(client_session);
    server.registerSession(server_session, service);
    alignas(16) std::array<u8, 0x2000> tls{};
    auto control = [&](u32 command) {
        tls.fill(0);
        auto request = hipc::makeRequest(
            tls.data(),
            {.type = static_cast<u32>(hipc::cmif::CommandType::Control),
             .num_data_words = 16});
        const hipc::cmif::InHeader input{hipc::cmif::IN_HEADER_MAGIC, 0,
                                         command, 0};
        std::memcpy(hipc::cmif::alignDataStart(request.data_words), &input,
                    sizeof(input));
        service->handleRequest(system, process,
                               reinterpret_cast<uptr>(tls.data()));
    };
    control(0); // Convert domain, including its self-reference.
    auto converted = hipc::parseRequest(tls.data());
    auto* converted_data = reinterpret_cast<u8*>(
        hipc::cmif::alignDataStart(converted.data.data_words));
    u32 root_id;
    std::memcpy(&root_id, converted_data + sizeof(hipc::cmif::OutHeader),
                sizeof(root_id));
    control(2); // Clone: two sessions must own two references.
    tls.fill(0);
    auto request = hipc::makeRequest(
        tls.data(), {.type = static_cast<u32>(hipc::cmif::CommandType::Request),
                     .num_data_words = 24});
    auto* data =
        reinterpret_cast<u8*>(hipc::cmif::alignDataStart(request.data_words));
    const hipc::cmif::DomainInHeader domain{
        .type = hipc::cmif::DomainCommandType::SendMessage,
        .data_size = sizeof(hipc::cmif::InHeader),
        .object_id = root_id};
    const hipc::cmif::InHeader input{hipc::cmif::IN_HEADER_MAGIC, 0, 0, 0};
    std::memcpy(data, &domain, sizeof(domain));
    std::memcpy(data + sizeof(domain), &input, sizeof(input));
    service->handleRequest(system, process, reinterpret_cast<uptr>(tls.data()));
    check(destroyed == 0, "Domain and child remain alive during requests");
    server.stop();
    server.stop();
    check(destroyed == 2, "Cloned domain and child are destroyed exactly once");
}

void testVideoCapture(System& system, kernel::Process* process) {
    video::Host1xState state;
    std::vector<std::pair<u32, u32>> writes;
    const std::array<u32, 7> packets{0x00003c00, 0x10100002, 0x101, 0x1234,
                                   0x10100002, 0xc0, 0x100};
    check(video::interpretHost1x(packets, state, [&](u32 method, u32 value) {
        writes.emplace_back(method, value);
    }).has_value(), "Host1x decodes NVDEC register packets");
    check(writes == std::vector<std::pair<u32, u32>>{{0x101, 0x1234}, {0xc0, 0x100}},
          "Host1x method selector routes arguments to the correct registers");
    const std::array<u32, 1> truncated{0x10100002};
    check(!video::interpretHost1x(truncated, state, [](u32, u32) {}),
          "Host1x rejects truncated packets before reading arguments");
    const auto directory = std::filesystem::temp_directory_path() /
        fmt::format("hydra-video-trace-test-{}", getpid());
    std::filesystem::remove_all(directory);
    vaddr_t guest;
    const auto host = process->createMemory(
        kernel::HEAP_REGION, 0x2000, kernel::MemoryType::Normal_4_0_0,
        kernel::MemoryPermission::ReadWrite, guest);
    const std::array<u8, 16> expected{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::memcpy(reinterpret_cast<void*>(host + 0xff8), expected.data(), expected.size());
    const auto handle = system.getGpu().createMap(0x2000);
    check(handle.has_value(), "Trace fixture nvmap allocation");
    check(system.getGpu().allocateMap(*handle, guest, false), "Trace fixture nvmap backing");
    class TraceNvdec final : public services::nvdrv::ioctl::ChannelBase {
        std::string_view videoEngineName() const override { return "nvdec"; }
    } channel;
    std::array<u32, 5> map_request{1, 0, 1, handle->getRaw(), 0};
    std::array<u32, 5> map_response{};
    services::nvdrv::ioctl::IoctlContext map_context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(map_request.data()), sizeof(map_request))),
        .out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(map_response.data()), sizeof(map_response)))};
    check(channel.ioctl(map_context, 0, 9) == services::nvdrv::NvResult::Success,
          "Host1x map buffer ioctl succeeds");
    const auto mapped_address = map_response[4];
    check(mapped_address != 0 && (mapped_address & 0xffff) == 0,
          "Host1x returns an aligned nonzero address");
    check(map_response[0] == 1 && map_response[2] == 1 && map_response[3] == handle->getRaw(),
          "Host1x map response preserves count, compression flag and handle");
    check(system.getGpu().resolveHost1x(mapped_address + 0xff8, 16) == guest + 0xff8,
          "Host1x address resolves into the guest nvmap");
    check(!system.getGpu().resolveHost1x(mapped_address + 0x1ff8, 16),
          "Host1x rejects reads past the nvmap");
    check(channel.ioctl(map_context, 0, 10) == services::nvdrv::NvResult::Success,
          "Host1x unmap buffer ioctl succeeds");
    check(channel.ioctl(map_context, 0, 9) == services::nvdrv::NvResult::Success && map_response[4] == mapped_address,
          "Host1x address stays stable across temporary unmap/remap");
    map_request[0] = 2;
    check(channel.ioctl(map_context, 0, 9) == services::nvdrv::NvResult::InvalidSize,
          "Host1x rejects a truncated map array");
    map_request[0] = 1;
    map_request[3] = 0;
    check(channel.ioctl(map_context, 0, 9) == services::nvdrv::NvResult::InvalidAddress,
          "Host1x rejects an invalid handle");
    std::array<u32, 7> submission{1, 0, 0, 0, handle->getRaw(), 0xff8, 4};
    services::nvdrv::ioctl::IoctlContext context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(submission.data()), sizeof(submission)))};
    const char* previous = std::getenv("HYDRA_DUMP_VIDEO");
    const std::optional<std::string> previous_directory = previous
        ? std::optional<std::string>(previous) : std::nullopt;
    setenv("HYDRA_DUMP_VIDEO", directory.c_str(), 1);
    video::traceIoctl("nvdec", context, 0x40044801);
    usize ioctl_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.path().filename().string().starts_with("ioctl-"))
            continue;
        ++ioctl_files;
        check(entry.file_size() == sizeof(submission), "Early video ioctl capture preserves its input");
    }
    check(ioctl_files == 1, "Video ioctl is captured before any submit");
    video::traceSubmission("nvdec", context);
    check(context.in_stream->getSeek() == 0, "Capture does not consume ioctl data");
    usize command_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find("-cmd-") == std::string::npos)
            continue;
        ++command_files;
        std::ifstream input(entry.path(), std::ios::binary);
        std::array<u8, 16> actual{};
        input.read(reinterpret_cast<char*>(actual.data()),
                   static_cast<std::streamsize>(actual.size()));
        check(input.good() && actual == expected,
              "Captured command buffer crosses a guest page correctly");
    }
    check(command_files == 1, "One command buffer is captured");
    submission[5] = 0x2000; // Offset beyond the nvmap's backing range.
    video::traceSubmission("nvdec", context);
    command_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().string().find("-cmd-") != std::string::npos)
            ++command_files;
    check(command_files == 1, "Out-of-bounds nvmap command is not read");
    // Drive a real NVDEC submit ioctl through the register interpreter and
    // resolve the picture/bitstream through the addresses returned by map.
    const u32 packet_size = expected.size();
    std::memset(reinterpret_cast<void*>(host + 0x200), 0, 0x2fc);
    std::memcpy(reinterpret_cast<void*>(host + 0x248), &packet_size, sizeof(packet_size));
    std::memcpy(reinterpret_cast<void*>(host + 0xf00), expected.data(), expected.size());
    const std::array<u32, 13> engine_commands{
        0x00003c00, 0x10100002, 0x80, 3,
        0x10100002, 0x101, (mapped_address + 0x200) >> 8,
        0x10100002, 0x102, (mapped_address + 0xf00) >> 8,
        0x10100002, 0xc0, 0x100};
    std::memcpy(reinterpret_cast<void*>(host + 0x1800), engine_commands.data(), sizeof(engine_commands));
    submission[5] = 0x1800;
    submission[6] = engine_commands.size();
    check(channel.ioctl(context, 0, 1) == services::nvdrv::NvResult::Success,
          "NVDEC submit reads its command buffer and interprets engine registers");
    usize packet_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.path().filename().string().ends_with("-h264.bin"))
            continue;
        ++packet_files;
        std::ifstream input(entry.path(), std::ios::binary);
        std::array<u8, 16> actual{};
        input.read(reinterpret_cast<char*>(actual.data()), actual.size());
        check(input.good() && actual == expected,
              "NVDEC register addresses resolve to the actual guest picture and bitstream");
    }
    check(packet_files == 1, "NVDEC execution captures the bitstream selected by its picture context");
    if (previous_directory)
        setenv("HYDRA_DUMP_VIDEO", previous_directory->c_str(), 1);
    else
        unsetenv("HYDRA_DUMP_VIDEO");
    system.getGpu().freeMap(*handle);
    check(!system.getGpu().resolveHost1x(mapped_address, 1), "Freed nvmap loses its Host1x address");
    std::filesystem::remove_all(directory);
}

#ifdef HYDRA_FFMPEG_VIDEO
void testNvdecWriteback(System& system, kernel::Process* process) {
    std::ifstream input(std::filesystem::path(HYDRA_VIDEO_FIXTURE_DIR) / "h264.packets", std::ios::binary);
    std::array<char, 4> magic{};
    input.read(magic.data(), 4);
    check(std::string_view(magic.data(), 4) == "HYVP", "NVDEC test fixture magic");
    auto readWord = [&]() {
        std::array<u8, 4> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), 4);
        check(input.good(), "NVDEC fixture is complete");
        return u32(bytes[0]) | (u32(bytes[1]) << 8) | (u32(bytes[2]) << 16) | (u32(bytes[3]) << 24);
    };
    const auto count = readWord();
    check(count == 3, "NVDEC fixture has three frames");
    std::vector<std::vector<u8>> packets;
    for (u32 i = 0; i < count; ++i) {
        const auto size = readWord();
        check(size < 4096, "NVDEC fixture packet bound");
        auto& packet = packets.emplace_back(size);
        input.read(reinterpret_cast<char*>(packet.data()), size);
        check(input.good(), "NVDEC fixture packet data");
    }
    for (const bool linear : {true, false}) {
        vaddr_t guest;
        const auto host = process->createMemory(kernel::HEAP_REGION, 0x10000,
            kernel::MemoryType::Normal_4_0_0, kernel::MemoryPermission::ReadWrite, guest);
        std::memset(reinterpret_cast<void*>(host), 0, 0x10000);
        const auto handle = system.getGpu().createMap(0x10000);
        check(handle && system.getGpu().allocateMap(*handle, guest, false), "NVDEC writeback nvmap");
        const auto base = system.getGpu().mapHost1x(*handle);
        check(base.has_value(), "NVDEC writeback address");
        services::nvdrv::ioctl::NvHostNvDec channel;
        auto word = [&](usize offset, u32 value) {
            std::memcpy(reinterpret_cast<void*>(host + 0x100 + offset), &value, 4);
        };
        word(0x60, 1); word(0x64, 2); word(0x68, 1);
        word(0x70, 1); word(0x78, 2); word(0x80, 1); word(0x88, 1);
        word(0x8c, 64); word(0x90, 64);
        std::memset(reinterpret_cast<void*>(host + 0x2c0), 16, 0xe0);
        for (u32 index = 0; index < count; ++index) {
            const u64 flags = 2 | 4 | (1ull << 12) | (56ull << 16) |
                (30ull << 22) | (30ull << 27) | (2ull << 32) |
                (u64(index) << 34) | (u64(index) << 46) | (u64(linear) << 63);
            word(0xb0, u32(flags)); word(0xb4, u32(flags >> 32));
            word(0x48, u32(packets[index].size()));
            std::memcpy(reinterpret_cast<void*>(host + 0x1000), packets[index].data(), packets[index].size());
            const std::array<u32, 19> commands{
                0x00003c00, 0x10100002, 0x80, 3,
                0x10100002, 0x101, (*base + 0x100) >> 8,
                0x10100002, 0x102, (*base + 0x1000) >> 8,
                0x10100002, 0x10c + index, (*base + 0x4000 + index * 0x2000) >> 8,
                0x10100002, 0x11d + index, (*base + 0x5000 + index * 0x2000) >> 8,
                0x10100002, 0xc0, 0x100};
            std::memcpy(reinterpret_cast<void*>(host + 0x2000), commands.data(), sizeof(commands));
            std::array<u32, 7> submission{1, 0, 0, 0, handle->getRaw(), 0x2000, commands.size()};
            services::nvdrv::ioctl::IoctlContext context{
                .system = system, .process = process,
                .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(submission.data()), sizeof(submission)))};
            check(channel.ioctl(context, 0, 1) == services::nvdrv::NvResult::Success,
                  "Real NVDEC submit decodes independently encoded H264 packets");
        }
        std::array<u8, 1024> y{};
        std::array<u8, 512> uv{};
        if (linear) {
            std::memcpy(y.data(), reinterpret_cast<const void*>(host + 0x4000), y.size());
            std::memcpy(uv.data(), reinterpret_cast<const void*>(host + 0x5000), uv.size());
        } else {
            hw::tegra_x1::gpu::convertBlockLinearToLinear(64, 64, 1024, 16, 1, 0, 0,
                reinterpret_cast<const u8*>(host + 0x4000), y.data());
            hw::tegra_x1::gpu::convertBlockLinearToLinear(64, 64, 512, 8, 1, 0, 0,
                reinterpret_cast<const u8*>(host + 0x5000), uv.data());
        }
        for (usize row = 0; row < 16; ++row)
            for (usize column = 0; column < 32; ++column)
                check(y[row * 64 + column] >= 79 && y[row * 64 + column] <= 83,
                      "Decoded luma is written into the original guest surface");
        for (usize row = 0; row < 8; ++row)
            for (usize column = 0; column < 32; column += 2)
                check(uv[row * 64 + column] >= 88 && uv[row * 64 + column] <= 92 &&
                      uv[row * 64 + column + 1] >= 238 && uv[row * 64 + column + 1] <= 242,
                      "Decoded interleaved chroma survives linear and block-linear writeback");
        // Consume the selected decoded surface on a distinct VIC channel,
        // crop 16 to 8 rows, and convert UV to the game's requested VU order.
        auto configWord = [&](usize offset, u32 value) {
            std::memcpy(reinterpret_cast<void*>(host + 0x3000 + offset), &value, 4);
        };
        configWord(0x18, 31u << 16); configWord(0x1c, 15u << 16);
        configWord(0x20, 68 | (linear ? 0 : (1u << 11) | (1u << 15)));
        configWord(0x24, 31 | (7u << 14));
        configWord(0x28, 63 | (7u << 14)); configWord(0x2c, 31 | (3u << 14));
        configWord(0x90, 0x101);
        configWord(0xb4, 31u << 16); configWord(0xbc, 15u << 16);
        configWord(0xc0, 31u << 16); configWord(0xc4, 15u << 16);
        configWord(0xd0, 68);
        const std::array<u32, 16> vic_commands{
            0x00001740, 0x10100002, 0x100, (*base + 0x4000) >> 8,
            0x10100002, 0x1c2, (*base + 0x3000) >> 8,
            0x10100002, 0x1c8, (*base + 0xa000) >> 8,
            0x10100002, 0x1c9, (*base + 0xb000) >> 8,
            0x10100002, 0xc0, 0x100};
        std::memcpy(reinterpret_cast<void*>(host + 0xc000), vic_commands.data(), sizeof(vic_commands));
        services::nvdrv::ioctl::IoctlContext vic_context{.system = system, .process = process};
        services::nvdrv::ioctl::NvHostVic vic;
        std::array<u32, 2> sync_request{}, sync_response{};
        services::nvdrv::ioctl::IoctlContext sync_context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(sync_request.data()), sizeof(sync_request))),
            .out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(sync_response.data()), sizeof(sync_response)))};
        check(vic.ioctl(sync_context, 0, 2) == services::nvdrv::NvResult::Success && sync_response[1] != 0,
              "Video channel owns an independent nonzero syncpoint");
        const auto syncpoint = sync_response[1];
        // A recycled counter keeps the value of its previous owner
        const auto syncpoint_start = system.getGpu().getVideoSyncpoints().read(syncpoint).value();
        std::array<u32, 13> fenced_submission{1, 0, 1, 1, handle->getRaw(), 0xc000,
            vic_commands.size(), syncpoint, 2, 0xffffffff, 0xffffffff, 0xffffffff, 0};
        std::array<u32, 13> fenced_response{};
        vic_context.in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(fenced_submission.data()), sizeof(fenced_submission)));
        vic_context.out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(fenced_response.data()), sizeof(fenced_response)));
        check(vic.ioctl(vic_context, 0, 1) == services::nvdrv::NvResult::Success,
              "VIC consumes the NVDEC surface selected by guest commands");
        check(fenced_response[12] == syncpoint_start + 2 && fenced_response[4] == handle->getRaw(),
              "Successful submit returns an increasing fence and preserves command descriptors");
        services::nvdrv::ioctl::NvHostCtrl control;
        auto wait = [&](u32 threshold) {
            std::array<u32, 4> request{syncpoint, threshold, 0, 0}, response{};
            services::nvdrv::ioctl::IoctlContext wait_context{
                .system = system, .process = process,
                .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(request.data()), sizeof(request))),
                .out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(response.data()), sizeof(response)))};
            return control.ioctl(wait_context, 0, 0x1e);
        };
        check(wait(syncpoint_start + 2) == services::nvdrv::NvResult::Success &&
                  wait(syncpoint_start + 3) == services::nvdrv::NvResult::Timeout,
              "Syncpoint waits report actual completion rather than succeeding with zero");
        configWord(0x20, 29); // Unsupported 24-bit output must not signal completion.
        check(vic.ioctl(vic_context, 0, 1) == services::nvdrv::NvResult::NotSupported &&
              system.getGpu().getVideoSyncpoints().read(syncpoint) == syncpoint_start + 2,
              "A failed VIC job leaves its fence unsignalled");
        std::array<u8, 1024> vic_y{}, vic_uv{};
        if (linear) {
            std::memcpy(vic_y.data(), reinterpret_cast<const void*>(host + 0xa000), 512);
            std::memcpy(vic_uv.data(), reinterpret_cast<const void*>(host + 0xb000), 256);
        } else {
            hw::tegra_x1::gpu::convertBlockLinearToLinear(64, 64, 1024, 8, 1, 1, 0,
                reinterpret_cast<const u8*>(host + 0xa000), vic_y.data());
            hw::tegra_x1::gpu::convertBlockLinearToLinear(64, 64, 1024, 4, 1, 1, 0,
                reinterpret_cast<const u8*>(host + 0xb000), vic_uv.data());
        }
        for (usize row = 0; row < 8; ++row)
            for (usize column = 0; column < 32; ++column)
                check(vic_y[row * 64 + column] == y[row * 64 + column],
                      "VIC crops and preserves decoded luma in pitch and block-linear output");
        for (usize row = 0; row < 4; ++row)
            for (usize column = 0; column < 32; column += 2)
                check(vic_uv[row * 64 + column] == uv[row * 64 + column] &&
                      vic_uv[row * 64 + column + 1] == uv[row * 64 + column + 1],
                      "VIC keeps decoded NV12 chroma order for Y8___U8V8_N420 output");
        // Limited-range BT.601 conversion, including negative coefficients,
        // translation, nonzero matrix shift, alpha and packed channel order.
        configWord(0xa0, 1023u << 10); configWord(0xa4, 1023);
        const std::array<std::array<std::int32_t, 3>, 4> columns{{
            {298 * 8, 298 * 8, 298 * 8}, {0, -100 * 8, 516 * 8},
            {409 * 8, -208 * 8, 0},
            {-64 * 298 - 512 * 409, -64 * 298 + 512 * 308, -64 * 298 - 512 * 516}}};
        for (usize column = 0; column < columns.size(); ++column) {
            u64 packed = 0;
            for (usize component = 0; component < 3; ++component)
                packed |= (u64(columns[column][component]) & 0xfffff) << (component * 20);
            if (column == 0) packed |= 3ull << 60;
            if (column == 1) packed |= 1ull << 63;
            configWord(0xf0 + column * 8, u32(packed));
            configWord(0xf4 + column * 8, u32(packed >> 32));
        }
        for (const u32 format : {31u, 32u, 35u}) {
            configWord(0x20, format | (linear ? 0 : (1u << 11) | (1u << 15)));
            check(vic.ioctl(vic_context, 0, 1) == services::nvdrv::NvResult::Success,
                  "VIC matrix conversion completes a packed RGB job");
            std::array<u8, 2048> rgba{};
            if (linear)
                std::memcpy(rgba.data(), reinterpret_cast<const void*>(host + 0xa000), rgba.size());
            else
                hw::tegra_x1::gpu::convertBlockLinearToLinear(256, 256, 4096, 8, 1, 1, 0,
                    reinterpret_cast<const u8*>(host + 0xa000), rgba.data());
            for (usize row = 0; row < 8; ++row)
                for (usize column = 0; column < 32; ++column) {
                    const auto* pixel = rgba.data() + row * 256 + column * 4;
                    check(pixel[format == 32 ? 2 : 0] >= 250 && pixel[1] <= 3 &&
                          pixel[format == 32 ? 0 : 2] <= 3 && pixel[3] == 255,
                          "VIC decoded red survives matrix, RGB order, alpha and surface layout");
                }
        }
        system.getGpu().freeMap(*handle);
        check(!system.getGpu().getVideoFrame(*base + 0x4000),
              "Freeing a video nvmap releases its cached decoded picture");
    }
}
#endif

void testConditionVariableFlag(System& system, kernel::Process* process) {
    auto& kernel = system.getOs().getKernel();
    alignas(4) std::array<u32, 3> words{}; // Two independently owned mutexes and one shared CV.
    std::array<std::promise<kernel::result_t>, 2> results;
    std::array<std::promise<void>, 2> started;
    std::array<std::future<kernel::result_t>, 2> futures{results[0].get_future(), results[1].get_future()};
    std::array<kernel::HostThread*, 2> threads{};
    std::array<hydra::Handle, 2> handles{};
    for (usize index = 0; index < 2; ++index) {
        threads[index] = new kernel::HostThread(process, 0x20,
            [&, index](const kernel::should_stop_fn_t&) {
                atomicStore(&words[index], handles[index].getRaw());
                started[index].set_value();
                const auto result = kernel.waitProcessWideKeyAtomic(process, kernel::tls_current_thread,
                    reinterpret_cast<uptr>(&words[index]), reinterpret_cast<uptr>(&words[2]),
                    handles[index], 2'000'000'000);
                if (result == RESULT_SUCCESS)
                    kernel.arbitrateUnlock(kernel::tls_current_thread, reinterpret_cast<uptr>(&words[index]));
                results[index].set_value(result);
            });
        handles[index] = process->addHandleNoRetain(threads[index]);
        threads[index]->start();
        started[index].get_future().wait();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        // The CV flag is set and this mutex is released after queue insertion.
        while ((atomicLoad(&words[2]) != 1 || atomicLoad(&words[index]) != 0) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        check(atomicLoad(&words[2]) == 1 && atomicLoad(&words[index]) == 0,
              "Kernel publishes the CV waiter flag before releasing the mutex");
    }
    u64 woken = 0;
    check(kernel.signalProcessWideKey(process, reinterpret_cast<uptr>(&words[2]), 1, &woken) == RESULT_SUCCESS &&
          woken == 1 && atomicLoad(&words[2]) == 1,
          "Waking one of two waiters preserves the SDK waiter flag");
    woken = 0;
    check(kernel.signalProcessWideKey(process, reinterpret_cast<uptr>(&words[2]), -1, &woken) == RESULT_SUCCESS &&
          woken == 1 && atomicLoad(&words[2]) == 0,
          "Waking the final CV waiter clears its guest flag");
    for (usize index = 0; index < 2; ++index) {
        check(futures[index].wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
              futures[index].get() == RESULT_SUCCESS,
              "SDK-style signals resume both condition-variable waiters");
        threads[index]->stopAndJoin();
    }
}

void testAudioMixUpdates(System& system, kernel::Process* process) {
    namespace hipc = kernel::hipc;
    vaddr_t guest;
    const auto host = process->createMemory(
        kernel::HEAP_REGION, 0x6000, kernel::MemoryType::Normal_4_0_0,
        kernel::MemoryPermission::ReadWrite, guest);
    auto* input = reinterpret_cast<u8*>(host);
    auto descriptor = [](vaddr_t address, u32 size) {
        hipc::BufferDescriptor out{};
        out.size_low = size;
        out.address_low = static_cast<u32>(address);
        out.address_mid = static_cast<u32>((address >> 32) & 0xf);
        out.address_high = static_cast<u32>((address >> 36) & 0x3fffff);
        return out;
    };
    for (char revision : {'6', '7'}) {
        services::audio::AudioRendererParameters params{};
        params.sample_rate = 32000;
        params.sample_count = 160;
        params.mix_buffer_count = 2;
        params.submix_count = 1;
        params.revision = makeMagic4('R', 'E', 'V', revision);
        services::audio::IAudioRenderer renderer(system, params, 0);
        auto call = [&](i32 count, u32 id, bool truncated) {
            std::memset(input, 0, 0x2000);
            const u32 prefix = revision == '7' ? 0x20u : 0u;
            const u32 mix_size = prefix + (count > 0 ? 0x930u : 0u) - (truncated ? 1u : 0u);
            const u32 total = 0x40 + mix_size;
            std::memcpy(input, &params.revision, 4);
            std::memcpy(input + 0x18, &mix_size, 4);
            std::memcpy(input + 0x3c, &total, 4);
            if (prefix)
                std::memcpy(input + 0x44, &count, 4);
            if (count > 0)
                std::memcpy(input + 0x40 + prefix + 0x10, &id, 4);
            alignas(16) std::array<u8, 0x2000> tls{};
            const auto request = hipc::makeRequest(
                tls.data(), {.type = static_cast<u32>(hipc::cmif::CommandType::Request),
                             .num_send_buffers = 1, .num_recv_buffers = 2, .num_data_words = 8});
            request.send_buffers[0] = descriptor(guest, total);
            request.recv_buffers[0] = descriptor(guest + 0x2000, 0x2000);
            request.recv_buffers[1] = descriptor(guest + 0x4000, 0x2000);
            const hipc::cmif::InHeader command{hipc::cmif::IN_HEADER_MAGIC, 0, 4, 0};
            std::memcpy(hipc::cmif::alignDataStart(request.data_words), &command, sizeof(command));
            renderer.handleRequest(system, process, reinterpret_cast<uptr>(tls.data()));
            const auto response = hipc::parseRequest(tls.data());
            hipc::cmif::OutHeader result;
            std::memcpy(&result, hipc::cmif::alignDataStart(response.data.data_words), sizeof(result));
            return result.result;
        };
        check(call(1, 0, false) == RESULT_SUCCESS, "Legacy and REV7 mix payloads are accepted");
        check(call(1, 0, true) != RESULT_SUCCESS, "Truncated mix payload is rejected");
        if (revision == '7') {
            check(call(0, 0, false) == RESULT_SUCCESS, "Empty sparse update preserves existing mixes");
            check(call(1, 2, false) != RESULT_SUCCESS, "Out-of-range sparse mix ID is rejected");
            check(call(-1, 0, false) != RESULT_SUCCESS, "Negative sparse mix count is rejected");
        }
    }
}

void testAddOnContentEvent(System& system, kernel::Process* process) {
    namespace hipc = kernel::hipc;
    std::array<hydra::Handle, 2> handles;
    {
        services::aocsrv::IAddOnContentManager service;
        for (auto& handle : handles) {
            alignas(16) std::array<u8, 0x2000> tls{};
            const auto request = hipc::makeRequest(
                tls.data(), {.type = static_cast<u32>(hipc::cmif::CommandType::Request),
                             .num_data_words = 8});
            auto* data = hipc::cmif::alignDataStart(request.data_words);
            const hipc::cmif::InHeader input{hipc::cmif::IN_HEADER_MAGIC, 0, 8, 0};
            std::memcpy(data, &input, sizeof(input));
            service.handleRequest(system, process, reinterpret_cast<uptr>(tls.data()));
            const auto response = hipc::parseRequest(tls.data());
            hipc::cmif::OutHeader output;
            std::memcpy(&output, hipc::cmif::alignDataStart(response.data.data_words), sizeof(output));
            check(output.result == RESULT_SUCCESS && response.meta.num_copy_handles == 1,
                  "DLC event command returns success and one copied handle");
            handle = response.data.copy_handles[0];
        }
        check(handles[0] != handles[1] &&
                  process->getHandle<kernel::Event>(handles[0]) ==
                      process->getHandle<kernel::Event>(handles[1]),
              "Repeated DLC event requests share the event through independent handles");
    }
    auto* event = process->getHandle<kernel::Event>(handles[0]);
    check(event != nullptr && !event->clear(),
          "Static DLC event remains valid and unsignalled after service destruction");
    check(process->freeHandle(handles[0]) && process->freeHandle(handles[1]),
          "Both copied DLC event handles can be closed");
}

void testClockRequests(System& system, kernel::Process* process) {
    namespace hipc = kernel::hipc;
    services::mmnv::IRequest service;
    auto call = [&](u32 command, std::initializer_list<u32> arguments) {
        alignas(16) std::array<u8, 0x2000> tls{};
        const auto request = hipc::makeRequest(
            tls.data(), {.type = static_cast<u32>(hipc::cmif::CommandType::Request),
                         .num_data_words = 16});
        auto* data = reinterpret_cast<u8*>(hipc::cmif::alignDataStart(request.data_words));
        const hipc::cmif::InHeader input{hipc::cmif::IN_HEADER_MAGIC, 0, command, 0};
        std::memcpy(data, &input, sizeof(input));
        std::memcpy(data + sizeof(input), arguments.begin(), arguments.size() * sizeof(u32));
        service.handleRequest(system, process, reinterpret_cast<uptr>(tls.data()));
        const auto response = hipc::parseRequest(tls.data());
        const bool has_output = command == 3 || command == 4 || command == 7;
        check(response.meta.num_data_words == (has_output ? 9u : 8u),
              "CMIF response advertises its payload plus sixteen padding bytes");
        data = reinterpret_cast<u8*>(hipc::cmif::alignDataStart(response.data.data_words));
        const auto* raw_end = reinterpret_cast<const u8*>(response.data.data_words) +
                              response.meta.num_data_words * sizeof(u32);
        check(data + sizeof(hipc::cmif::OutHeader) + (has_output ? sizeof(u32) : 0) <= raw_end,
              "Clock response payload fits in the advertised HIPC data region");
        hipc::cmif::OutHeader header;
        u32 value;
        std::memcpy(&header, data, sizeof(header));
        std::memcpy(&value, data + sizeof(header), sizeof(value));
        return std::pair{header.result, value};
    };
    const auto first = call(4, {5, 8, 0});
    const auto second = call(4, {5, 8, 0});
    check(first.first == RESULT_SUCCESS && second.first == RESULT_SUCCESS &&
              first.second != second.second, "Clock requests have independent IDs");
    check(call(6, {first.second, 12000000, 0xffffffffu}).first == RESULT_SUCCESS,
          "Clock set accepts an infinite signed timeout");
    check(call(7, {first.second}).second == 12000000,
          "Clock get returns the applied frequency instead of zero");
    check(call(6, {first.second, 0xfffffc18u, 0xffffffffu}).first == RESULT_SUCCESS,
          "NVDEC accepts the driver's maximum frequency probe");
    check(call(7, {first.second}).second == 408000000,
          "Maximum frequency probe returns a bounded virtual NVDEC clock");
    check(call(7, {second.second}).second == 0, "Clock requests remain isolated");
    check(call(5, {first.second}).first == RESULT_SUCCESS, "Clock request finalizes");
    check(call(7, {first.second}).first != RESULT_SUCCESS, "Finalized clock request is invalid");
    check(call(0, {5, 8, 0}).first == RESULT_SUCCESS, "Legacy clock initializes");
    check(call(2, {5, 12000000, 0xffffffffu}).first == RESULT_SUCCESS,
          "Legacy clock setting succeeds");
    check(call(3, {5}).second == 12000000, "Legacy clock get returns its setting");
    check(call(1, {5}).first == RESULT_SUCCESS, "Legacy clock finalizes");
}

void testAudioOutShutdown(System& system, kernel::Process* process) {
    namespace hipc = kernel::hipc;
    for (int iteration = 0; iteration < 8; ++iteration) {
        services::audio::IAudioOut service(system, audio::PcmFormat::Int16,
                                         48000, 2);
        alignas(16) std::array<u8, 0x2000> tls{};
        const auto request = hipc::makeRequest(
            tls.data(), {.type = static_cast<u32>(hipc::cmif::CommandType::Request),
                         .num_data_words = 16});
        const hipc::cmif::InHeader input{hipc::cmif::IN_HEADER_MAGIC, 0, 1, 0};
        std::memcpy(hipc::cmif::alignDataStart(request.data_words), &input,
                    sizeof(input));
        service.handleRequest(system, process, reinterpret_cast<uptr>(tls.data()));
        // Destruction must drain a started host stream before service state dies.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void testVideoEvents(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostCtrl control;
    auto call = [&](u32 nr, std::span<u32> input, std::span<u32> output) {
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(input.data()), input.size_bytes())),
            .out_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(output.data()), output.size_bytes()))};
        return control.ioctl(context, 0, nr);
    };
    std::array<u32, 1> slot{0}, registered{};
    check(call(0x1f, slot, registered) == nv::NvResult::Success,
          "Cannot register video event");
    kernel::Event* event = nullptr;
    check(control.queryEvent(0, event) == nv::NvResult::Success && event != nullptr,
          "Cannot query registered video event");
    auto& counters = system.getGpu().getVideoSyncpoints();
    const auto id = counters.allocate();
    check(id.has_value(), "Cannot allocate video syncpoint");
    std::array<u32, 4> request{*id, 2, 0xffffffff, 0}, response{};
    check(call(0x1e, request, response) == nv::NvResult::Timeout,
          "Pending async video wait returned wrong status");
    check(counters.complete(*id, 1).has_value() && !event->clear(),
          "Video event signalled before its threshold");
    check(counters.complete(*id, 1).has_value() && event->clear(),
          "Completing video counter did not signal its waiting event");
    check(call(0x1e, request, response) == nv::NvResult::Success && event->clear(),
          "Already-completed async video wait did not signal its event");
    request[1] = 3;
    check(call(0x1e, request, response) == nv::NvResult::Timeout,
          "Cannot arm another video event wait");
    std::array<u32, 1> encoded_slot{0x10000000}, signal_output{};
    check(call(0x1c, encoded_slot, signal_output) == nv::NvResult::Success &&
              counters.complete(*id, 1).has_value() && !event->clear(),
          "EventSignal cancels a timed-out wait without signaling completion");

    std::promise<void> callback_started, release_callback;
    auto callback_ready = callback_started.get_future();
    auto release_ready = release_callback.get_future().share();
    counters.onReached(*id, 4, [&] {
        callback_started.set_value();
        release_ready.wait_for(std::chrono::seconds(2));
    });
    request[1] = 4;
    check(call(0x1e, request, response) == nv::NvResult::Timeout,
          "Pending wait requests the caller's event-wait path with result 5");
    auto completing = std::async(std::launch::async, [&] { return counters.complete(*id, 1); });
    check(callback_ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "Hold an extracted completion callback before it signals the event");
    check(call(0x1c, encoded_slot, signal_output) == nv::NvResult::Success,
          "Cancel a wait whose old callback has already left the counter queue");
    request[1] = 5;
    check(call(0x1e, request, response) == nv::NvResult::Timeout,
          "Rearm the same event for the next threshold");
    release_callback.set_value();
    check(completing.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
              completing.get().has_value() && !event->clear(),
          "A stale extracted callback cannot signal the rearmed event");
    check(counters.complete(*id, 1).has_value() && event->clear(),
          "Only the new threshold signals the rearmed event");
    request[1] = 6;
    check(call(0x1e, request, response) == nv::NvResult::Timeout,
          "Arm a final pending wait before unregistering");
    check(call(0x20, slot, registered) == nv::NvResult::Success &&
              counters.complete(*id, 1).has_value(),
          "Unregistering a pending video event did not safely cancel it");
    std::cout << "PASS: asynchronous video event completion, rearm and cancellation\n";
}

void bindTestChannel(services::nvdrv::ioctl::NvHostGpu& channel,
                     kernel::Process* process) {
    // Older decoder fixtures install host mappings directly in this test MMU.
    const auto space = std::shared_ptr<hw::tegra_x1::gpu::GMmu>(
        &process->getGMmu(), [](auto*) {});
    check(channel.bindAddressSpace(*process, space) == services::nvdrv::NvResult::Success,
          "Bind the decoder fixture's explicit address space");
}

void testGpuAddressSpaceInput(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostAsGpu address_space;
    auto& gpu = system.getGpu();
    nv::ioctl::NvHostGpu mapped_channel;
    std::array<u32, 1> channel_fd{1};
    nv::ioctl::IoctlContext binding{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(channel_fd.data()), sizeof(channel_fd))),
        .resolve_fd = [&](u32 fd) -> nv::ioctl::FdBase* {
            return fd == 1 ? &mapped_channel : nullptr;
        }};
    check(address_space.ioctl(binding, 0x41, 1) == nv::NvResult::Success,
          "Bind the mapping fixture's actual AS FD");
    auto& gmmu = *mapped_channel.getChannelState()->address_space;
    vaddr_t guest;
    const auto host = process->createMemory(kernel::HEAP_REGION, 0x20000,
        kernel::MemoryType::Normal_4_0_0, kernel::MemoryPermission::ReadWrite, guest);
    std::memset(reinterpret_cast<void*>(host), 0x12, 0x10000);
    std::memset(reinterpret_cast<void*>(host + 0x10000), 0x34, 0x10000);
    const auto handle = gpu.createMap(0x20000).value();
    check(gpu.allocateMap(handle, guest, true), "Allocate mapping-test nvmap");
    constexpr u64 TARGET = 0x70000000;
    const auto map = [&](u32 memory, u64 offset, u64 size, u64 target,
                         usize in_size = 40, usize out_size = 40) {
        std::array<u8, 41> input{}, output{};
        output.fill(0xa5);
        const u32 flags = 1;
        std::memcpy(input.data() + 1, &flags, 4);
        std::memcpy(input.data() + 9, &memory, 4);
        std::memcpy(input.data() + 17, &offset, 8);
        std::memcpy(input.data() + 25, &size, 8);
        std::memcpy(input.data() + 33, &target, 8);
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(input).first(in_size + 1)),
            .out_stream = ztd::io::MemoryStream(std::span(output).first(out_size + 1))};
        context.in_stream->seekBy(1);
        context.out_stream->seekBy(1);
        const auto result = address_space.ioctl(context, 0x41, 0x06);
        if (result != nv::NvResult::Success)
            check(std::all_of(output.begin(), output.end(),
                             [](u8 value) { return value == 0xa5; }),
                  "Rejected GPU mapping leaves the IPC response untouched");
        return result;
    };
    for (usize size = 0; size < 40; ++size) {
        check(map(handle.getRaw(), 0, 0x10000, TARGET, size) == nv::NvResult::InvalidSize,
              "MapBufferEx rejects every truncated input length");
        check(map(handle.getRaw(), 0, 0x10000, TARGET, 40, size) == nv::NvResult::InvalidSize,
              "MapBufferEx rejects every truncated output length");
    }
    check(map(handle.getRaw(), 0x10000, 0x10000, TARGET) == nv::NvResult::Success &&
              gmmu.readMappedBytes(TARGET, 16) == std::vector<u8>(16, 0x34),
          "Unaligned IPC maps the requested nvmap subrange");
    for (const auto& [offset, size] : std::array<std::pair<u64, u64>, 5>{
             {{0x20001, 1}, {0x10000, 0x10001}, {0x20000, 1},
              {0x10000, 0}, {0xfffffffffffffff0ull, 0x100}}})
        check(map(handle.getRaw(), offset, size, TARGET) == nv::NvResult::InvalidSize &&
                  gmmu.readMappedBytes(TARGET, 16) == std::vector<u8>(16, 0x34),
              "Out-of-allocation ranges cannot replace an existing GPU mapping");
    check(map(handle.getRaw(), 0, 0x10000, 0xfffffffffffffff0ull) == nv::NvResult::InvalidAddress,
          "Fixed GPU mapping address cannot wrap");
    const auto unallocated = gpu.createMap(0x10000).value();
    check(map(unallocated.getRaw(), 0, 0x10000, TARGET) == nv::NvResult::InvalidAddress,
          "An nvmap without CPU backing cannot become a GPU mapping");
    const auto hole = gpu.createMap(0x2000).value();
    check(gpu.allocateMap(hole, guest + 0x1f000, true) &&
              map(hole.getRaw(), 0, 0x2000, TARGET) == nv::NvResult::InvalidAddress,
          "A mapping with an unmapped CPU backing tail is rejected");
    // NVN pools of unaligned size: the guest maps the size rounded up to the big
    // page size, but only the nvmap allocation has memory behind it
    const auto padded = gpu.createMap(0x19000).value();
    constexpr u64 PADDED_TARGET = 0x71000000;
    check(gpu.allocateMap(padded, guest + 0x7000, true) &&
              map(padded.getRaw(), 0, 0x20000, PADDED_TARGET) == nv::NvResult::Success &&
              gmmu.readMappedBytes(PADDED_TARGET, 16) == std::vector<u8>(16, 0x12) &&
              gmmu.readMappedBytes(PADDED_TARGET + 0x18000, 16) == std::vector<u8>(16, 0x34),
          "A big-page-rounded mapping of an unaligned nvmap maps its backed part");
    check(map(padded.getRaw(), 0, 0x30000, PADDED_TARGET + 0x100000) == nv::NvResult::InvalidSize,
          "Rounding up to the big page size does not allow more than that");
    check(gpu.freeMap(padded), "Release padding-test nvmap");
    const auto remap = [&](std::span<const nv::ioctl::RemapOp> entries, usize bytes) {
        std::vector<u8> input(1 + entries.size_bytes());
        if (!entries.empty())
            std::memcpy(input.data() + 1, entries.data(), entries.size_bytes());
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(input).first(bytes + 1))};
        context.in_stream->seekBy(1);
        return address_space.ioctl(context, 0x41, 0x14);
    };
    const std::array<nv::ioctl::RemapOp, 2> invalid_batch{{
        {0, 0, 0, 0, u32(TARGET / 0x10000), 1},
        {0, 0, handle.getRaw(), 2, u32(TARGET / 0x10000 + 1), 1}}};
    check(remap(invalid_batch, sizeof(invalid_batch)) == nv::NvResult::InvalidSize &&
              gmmu.readMappedBytes(TARGET, 16) == std::vector<u8>(16, 0x34),
          "An invalid remap tail cannot apply an earlier unbind");
    const std::array<nv::ioctl::RemapOp, 1> valid_batch{{
        {0, 0, handle.getRaw(), 0, u32(TARGET / 0x10000), 1}}};
    for (usize size = 1; size < sizeof(nv::ioctl::RemapOp); ++size)
        check(remap(valid_batch, size) == nv::NvResult::InvalidSize,
              "Remap rejects incomplete descriptors");
    check(remap(valid_batch, sizeof(valid_batch)) == nv::NvResult::Success &&
              gmmu.readMappedBytes(TARGET, 16) == std::vector<u8>(16, 0x12),
          "Unaligned remap input respects the current IPC stream cursor");
    check(map(handle.getRaw(), 0, 0, TARGET + 0x100000) == nv::NvResult::Success &&
              gmmu.readMappedBytes(TARGET + 0x110000, 16) == std::vector<u8>(16, 0x34),
          "Zero mapping size at offset zero retains whole-nvmap behavior");
    check(gpu.freeMap(hole) && gpu.freeMap(unallocated) && gpu.freeMap(handle),
          "Release mapping-test nvmap handles");
    std::cout << "PASS: GPU mapping bounds, IPC sizes, backing holes and remap validation\n";
}

void testGpuAddressSpaceLifecycle(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostAsGpu address_space;
    auto& gpu = system.getGpu();
    nv::ioctl::NvHostGpu channel;
    std::array<u32, 1> channel_fd{1};
    nv::ioctl::IoctlContext binding{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(channel_fd.data()), sizeof(channel_fd))),
        .resolve_fd = [&](u32 fd) -> nv::ioctl::FdBase* {
            return fd == 1 ? &channel : nullptr;
        }};
    check(address_space.ioctl(binding, 0x41, 1) == nv::NvResult::Success,
          "Bind the lifecycle fixture's AS FD");
    auto& gmmu = *channel.getChannelState()->address_space;
    vaddr_t guest;
    const auto host = process->createMemory(kernel::HEAP_REGION, 0x20000,
        kernel::MemoryType::Normal_4_0_0, kernel::MemoryPermission::ReadWrite, guest);
    std::memset(reinterpret_cast<void*>(host), 0x12, 0x20000);
    const auto handle = gpu.createMap(0x20000).value();
    check(gpu.allocateMap(handle, guest, true), "Allocate lifecycle nvmap");

    // In/out argument blocks; the generic reader requires aligned storage.
    const auto call = [&](u32 nr, std::span<u64> words) {
        alignas(8) std::array<u64, 8> in{}, out{};
        std::copy(words.begin(), words.end(), in.begin());
        out = in;
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(in.data()), words.size_bytes())),
            .out_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(out.data()), words.size_bytes()))};
        const auto result = address_space.ioctl(context, 0x41, nr);
        std::copy_n(out.begin(), words.size(), words.begin());
        return result;
    };
    // AllocSpace: {pages | page_size << 32, flags, offset/align}
    const auto alloc = [&](u32 pages, u32 page_size, u32 flags, u64 offset, u64& result) {
        std::array<u64, 3> args{pages | (u64(page_size) << 32), flags, offset};
        const auto status = call(0x02, args);
        result = args[2];
        return status;
    };
    const auto free_space = [&](u64 offset, u32 pages, u32 page_size) {
        std::array<u64, 2> args{offset, pages | (u64(page_size) << 32)};
        return call(0x03, args);
    };
    const auto unmap = [&](u64 offset) {
        std::array<u64, 1> args{offset};
        return call(0x05, args);
    };
    // MapBufferEx: {flags|kind, handle|reserved, offset, size, address}
    const auto map = [&](u32 flags, u64 offset, u64 size, u64& address) {
        std::array<u64, 5> args{flags, handle.getRaw(), offset, size, address};
        const auto status = call(0x06, args);
        address = args[4];
        return status;
    };
    const auto bytes = [&](u64 address) { return gmmu.readMappedBytes(address, 16); };
    const std::vector<u8> zeros(16, 0), twelve(16, 0x12);
    constexpr u32 FIXED = 1, SPARSE = 2, PAGE = 0x10000;
    u64 out = 0;

    // Argument validation
    check(alloc(0, PAGE, 0, 0, out) == nv::NvResult::BadValue &&
              alloc(1, 0x1800, 0, 0, out) == nv::NvResult::BadValue &&
              alloc(1, PAGE, 0x80, 0, out) == nv::NvResult::BadValue &&
              alloc(1, PAGE, FIXED, 0x8001, out) == nv::NvResult::BadValue &&
              alloc(1, PAGE, FIXED, 0xffffffffffff0000ull, out) == nv::NvResult::BadValue,
          "AllocSpace rejects empty, odd page size, unknown flags and bad fixed offsets");

    // Plain reservations only claim address space; sparse ones read as zero.
    u64 plain = 0, sparse = 0x80000000;
    check(alloc(4, PAGE, 0, 0, plain) == nv::NvResult::Success && plain % PAGE == 0 &&
              bytes(plain).empty(),
          "A non-sparse reservation is address space only; reads are unbacked");
    u64 second = 0;
    check(alloc(1, PAGE, 0, 0, second) == nv::NvResult::Success &&
              (second >= plain + 4 * PAGE || second + PAGE <= plain),
          "A new reservation never overlaps an existing one");
    check(alloc(2, PAGE, FIXED | SPARSE, sparse, sparse) == nv::NvResult::Success &&
              bytes(0x80000000) == zeros && bytes(0x8001fff0) == zeros,
          "A sparse reservation reads as zero");
    u64 overlap = 0x80010000;
    check(alloc(1, PAGE, FIXED, overlap, overlap) == nv::NvResult::BadValue &&
              alloc(1, PAGE, FIXED, plain, overlap) == nv::NvResult::BadValue,
          "Fixed reservations cannot overlap existing reservations");

    // UnmapBuffer restores sparse zero backing and tolerates unknown addresses.
    u64 address = 0x80000000;
    check(map(FIXED, 0, PAGE, address) == nv::NvResult::Success && bytes(address) == twelve,
          "Map a buffer inside a sparse reservation");
    check(unmap(0x80000000) == nv::NvResult::Success && bytes(0x80000000) == zeros &&
              bytes(0x8000fff0) == zeros,
          "UnmapBuffer removes the mapping and restores sparse zeros");
    check(unmap(0x80000000) == nv::NvResult::Success && unmap(0x12340000) == nv::NvResult::Success,
          "Repeated or unknown UnmapBuffer succeeds without effect");

    // FreeSpace validates the reservation and keeps buffers mapped.
    address = 0x80000000;
    check(map(FIXED, 0, PAGE, address) == nv::NvResult::Success,
          "Map a buffer again before freeing its reservation");
    check(free_space(0x80000000, 1, PAGE) == nv::NvResult::BadValue &&
              free_space(0x80010000, 1, PAGE) == nv::NvResult::BadValue &&
              free_space(0x80000000, 2, 0x1800) == nv::NvResult::BadValue &&
              bytes(0x80010000) == zeros,
          "FreeSpace rejects mismatched offsets, sizes and page sizes");
    check(free_space(0x80000000, 2, PAGE) == nv::NvResult::Success &&
              bytes(0x80000000) == twelve && bytes(0x80010000).empty(),
          "FreeSpace drops sparse backing but keeps mapped buffers");
    check(free_space(0x80000000, 2, PAGE) == nv::NvResult::BadValue,
          "A reservation cannot be freed twice");
    check(unmap(0x80000000) == nv::NvResult::Success && bytes(0x80000000).empty(),
          "A buffer outliving its reservation unmaps to unbacked space");
    check(free_space(plain, 4, PAGE) == nv::NvResult::Success &&
              free_space(second, 1, PAGE) == nv::NvResult::Success,
          "Free non-sparse reservations");

    // Automatic placement respects reservations and reuses released space.
    u64 fixed_low = 0x20000;
    check(alloc(1, 0x20000, FIXED, fixed_low, fixed_low) == nv::NvResult::Success,
          "Reserve the start of the automatic range");
    u64 auto_a = 0, auto_b = 0;
    check(map(0, 0, PAGE, auto_a) == nv::NvResult::Success && auto_a >= 0x40000 &&
              map(0, 0x10000, PAGE, auto_b) == nv::NvResult::Success &&
              auto_b >= auto_a + 0x20000 && bytes(auto_a) == twelve,
          "Automatic mappings avoid reservations and each other");
    check(unmap(auto_a) == nv::NvResult::Success && bytes(auto_a).empty(), "Unmap automatic mapping");
    u64 reused = 0;
    check(map(0, 0, PAGE, reused) == nv::NvResult::Success && reused == auto_a,
          "Released address space is reused");

    // A fixed map replacing the middle of a mapping splits its record.
    constexpr u64 SPLIT = 0x90000000;
    u64 whole = SPLIT, middle = SPLIT + 0x4000;
    check(map(FIXED, 0, PAGE, whole) == nv::NvResult::Success &&
              map(FIXED, 0x10000, 0x1000, middle) == nv::NvResult::Success,
          "Map a buffer and overwrite its middle");
    check(unmap(SPLIT + 0x5000) == nv::NvResult::Success && bytes(SPLIT + 0x5000) .empty() &&
              bytes(SPLIT + 0x4000).size() == 16 && bytes(SPLIT).size() == 16,
          "The remainder after a replaced range stays individually unmappable");
    check(unmap(SPLIT) == nv::NvResult::Success && bytes(SPLIT).empty() &&
              bytes(SPLIT + 0x4000).size() == 16,
          "The head record only covers the bytes before the replacement");
    // NVN maps whole big pages of an nvmap that is smaller than one: allowed up
    // to the size rounded up to the big page, with readable backing for every
    // mapped page (the CTR init regression)
    {
        const auto small_map = [&](u64 base, u64 offset, u64 size, u64 target) {
            const auto small = gpu.createMap(0x8000).value();
            check(gpu.allocateMap(small, base, true), "Allocate small nvmap");
            std::array<u64, 5> args{FIXED, small.getRaw(), offset, size, target};
            const auto status = call(0x06, args);
            check(gpu.freeMap(small), "Release small nvmap");
            return status;
        };
        check(small_map(guest, 0, 0x10000, 0xb0000000) == nv::NvResult::Success &&
                  bytes(0xb0000000) == twelve,
              "A mapping rounded up to the big page of a smaller nvmap succeeds");
        check(small_map(guest, 0, 0x20000, 0xb0100000) == nv::NvResult::InvalidSize &&
                  small_map(guest, 0x1000, 0x10000, 0xb0200000) == nv::NvResult::InvalidSize &&
                  bytes(0xb0100000).empty() && bytes(0xb0200000).empty(),
              "Mappings beyond the nvmap's big-page limit are rejected");
        // Unreal Engine's NVN memory pools (e.g. 0x1e9000 bytes mapped as 0x1f0000)
        // end in alignment padding past the nvmap allocation: only the
        // allocation itself needs readable backing, otherwise the game aborts
        check(small_map(guest + 0x18000, 0, 0x10000, 0xb0300000) == nv::NvResult::Success &&
                  bytes(0xb0300000).size() == 16,
              "The rounded-up padding past the nvmap needs no guest backing");
        check(unmap(0xb0300000) == nv::NvResult::Success, "Unmap the padded mapping");
        check(unmap(0xb0000000) == nv::NvResult::Success, "Unmap the rounded mapping");
    }

    // SET_TIMESLICE (0xc004481d) is accepted and echoes the value
    {
        alignas(8) std::array<u32, 2> timeslice{1000, 0}, reply = timeslice;
        nv::ioctl::IoctlContext slice_context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(timeslice.data()), 4)),
            .out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(reply.data()), 4))};
        check(channel.ioctl(slice_context, 0x48, 0x1d) == nv::NvResult::Success && reply[0] == 1000,
              "SET_TIMESLICE succeeds");
    }
    // An nvmap handle created by another process cannot be mapped here
    int other_process = 0;
    const auto foreign = gpu.createMap(0x10000, &other_process).value();
    check(gpu.allocateMap(foreign, guest, true), "Allocate foreign nvmap");
    {
        std::array<u64, 5> args{FIXED, foreign.getRaw(), 0, 0x10000, 0xa0000000};
        check(call(0x06, args) == nv::NvResult::BadParameter &&
                  gmmu.readMappedBytes(0xa0000000, 16).empty(),
              "A handle owned by another process cannot be mapped into this address space");
    }
    check(gpu.freeMap(foreign), "Release foreign nvmap");
    uptr heap_base = 0;
    check(system.getOs().getKernel().setHeapSize(process, 0x100200000ull, heap_base) ==
              MAKE_RESULT(Svc, kernel::Error::InvalidSize) &&
              system.getOs().getKernel().setHeapSize(process, 0x1000, heap_base) ==
                  MAKE_RESULT(Svc, kernel::Error::InvalidSize),
          "SetHeapSize rejects unaligned sizes and sizes above 4GB");
    check(free_space(0x20000, 1, 0x20000) == nv::NvResult::Success &&
              gpu.freeMap(handle),
          "Release lifecycle fixtures");
    std::cout << "PASS: AS_GPU reservations, FreeSpace, UnmapBuffer and placement\n";
}

void testGpuBackingFollowsHeapResize(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostAsGpu address_space;
    nv::ioctl::NvHostGpu channel;
    std::array<u32, 1> channel_fd{1};
    nv::ioctl::IoctlContext binding{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(channel_fd.data()), sizeof(channel_fd))),
        .resolve_fd = [&](u32 fd) -> nv::ioctl::FdBase* {
            return fd == 1 ? &channel : nullptr;
        }};
    check(address_space.ioctl(binding, 0x41, 1) == nv::NvResult::Success,
          "Bind the heap fixture's AS FD");
    auto& gmmu = *channel.getChannelState()->address_space;

    process->resizeHeap(0x20000);
    const auto heap = kernel::HEAP_REGION.getBegin();
    auto* first = reinterpret_cast<u8*>(process->getHeapMemory()->getPtr());
    std::memset(first, 0x21, 0x20000);
    const auto handle = system.getGpu().createMap(0x20000).value();
    check(system.getGpu().allocateMap(handle, heap, true), "Allocate heap nvmap");
    alignas(8) std::array<u64, 5> args{1, handle.getRaw(), 0, 0x20000, 0x70000000};
    alignas(8) std::array<u64, 5> out = args;
    nv::ioctl::IoctlContext context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(args.data()), 40)),
        .out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(out.data()), 40))};
    check(address_space.ioctl(context, 0x41, 0x06) == nv::NvResult::Success &&
              gmmu.readMappedBytes(0x70000000, 16) == std::vector<u8>(16, 0x21),
          "Map the guest heap into the GPU address space");

    const auto old_ptr = process->getHeapMemory()->getPtr();
    process->resizeHeap(0x40000);
    auto* grown = reinterpret_cast<u8*>(process->getHeapMemory()->getPtr());
    check(process->getHeapMemory()->getPtr() != old_ptr && grown[0x1fff0] == 0x21 &&
              grown[0x20000] == 0,
          "Growing the heap keeps its contents and zero-fills the new tail");
    // The old block must still be valid memory for work that was queued before
    check(first[0x100] == 0x21, "Replaced heap memory stays alive while pinned");
    grown[0x10] = 0x66;
    check(gmmu.readMappedBytes(0x70000010, 1) == std::vector<u8>(1, 0x66) &&
              gmmu.readMappedBytes(0x70000000, 16)[1] == 0x21,
          "GPU mapping follows the heap to its new host memory");

    process->resizeHeap(0x10000);
    check(gmmu.readMappedBytes(0x70000000, 16).size() == 16 &&
              gmmu.readMappedBytes(0x70010000, 16).empty(),
          "Shrinking drops GPU mappings beyond the new heap size");
    check(system.getGpu().freeMap(handle), "Release heap nvmap");
    std::cout << "PASS: GPU backing follows and pins heap memory across resize\n";
}

void testGpuAddressSpaceBinding(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    namespace gpu = hw::tegra_x1::gpu;
    auto as_a = std::make_unique<nv::ioctl::NvHostAsGpu>();
    auto as_b = std::make_unique<nv::ioctl::NvHostAsGpu>();
    nv::ioctl::NvHostGpu a, b, shared, unbound;
    nv::ioctl::NvHostCtrl wrong_device;
    std::map<u32, nv::ioctl::FdBase*> fds{{1, &a}, {2, &b}, {3, &shared}, {4, &wrong_device}};
    const auto bind = [&](nv::ioctl::NvHostAsGpu& as, u32 fd,
                          kernel::Process* caller, usize size = 4) {
        std::array<u8, 5> input{};
        std::memcpy(input.data() + 1, &fd, 4);
        nv::ioctl::IoctlContext context{
            .system = system, .process = caller,
            .in_stream = ztd::io::MemoryStream(std::span(input).first(size + 1)),
            .resolve_fd = [&](u32 id) -> nv::ioctl::FdBase* {
                const auto it = fds.find(id);
                return it == fds.end() ? nullptr : it->second;
            }};
        context.in_stream->seekBy(1);
        return as.ioctl(context, 0x41, 1);
    };
    for (usize size = 0; size < 4; ++size)
        check(bind(*as_a, 1, process, size) == nv::NvResult::InvalidSize,
              "Truncated AS bind does not bind a channel");
    check(bind(*as_a, 99, process) == nv::NvResult::BadParameter &&
              bind(*as_a, 4, process) == nv::NvResult::BadParameter,
          "AS bind rejects missing FDs and non-GPU devices");
    check(bind(*as_a, 1, process) == nv::NvResult::Success &&
              bind(*as_b, 2, process) == nv::NvResult::Success &&
              bind(*as_a, 3, process) == nv::NvResult::Success,
          "Independent AS FDs bind channels and one AS can serve multiple channels");
    check(bind(*as_a, 1, process) == nv::NvResult::BadValue &&
              bind(*as_b, 1, process) == nv::NvResult::BadValue,
          "An already bound channel cannot be rebound, even to its existing AS");
    check(a.getChannelState()->address_space != b.getChannelState()->address_space &&
              a.getChannelState()->address_space == shared.getChannelState()->address_space,
          "Channel binding retains the selected AS identity");

    constexpr u64 COMMAND = 0x71000000;
    constexpr u64 TARGET = COMMAND + 0x1000;
    std::array<vaddr_t, 2> guests{};
    std::array<uptr, 2> hosts{};
    std::array<hydra::Handle, 2> handles{};
    for (usize i = 0; i < 2; ++i) {
        hosts[i] = process->createMemory(kernel::HEAP_REGION, 0x20000,
            kernel::MemoryType::Normal_4_0_0, kernel::MemoryPermission::ReadWrite, guests[i]);
        const std::array<u32, 5> words{
            (1u << 29) | (2u << 16) | 4u, 0, u32(TARGET),
            (1u << 29) | (1u << 16) | 0x1bu, u32(0x1111 * (i + 1))};
        std::memcpy(reinterpret_cast<void*>(hosts[i]), words.data(), sizeof(words));
        *reinterpret_cast<u32*>(hosts[i] + 0x1000) = 0;
        handles[i] = system.getGpu().createMap(0x20000).value();
        check(system.getGpu().allocateMap(handles[i], guests[i], true), "Allocate isolated nvmap");
    }
    const auto map = [&](nv::ioctl::NvHostAsGpu& as, hydra::Handle handle) {
        std::array<u64, 5> request{1, handle.getRaw(), 0, 0x20000, COMMAND};
        const auto bytes = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(bytes),
            .out_stream = ztd::io::MemoryStream(bytes)};
        return as.ioctl(context, 0x41, 6);
    };
    check(map(*as_a, handles[0]) == nv::NvResult::Success &&
              map(*as_b, handles[1]) == nv::NvResult::Success,
          "Different AS FDs map different backing at the same GPU VA");
    const auto submit = [&](nv::ioctl::NvHostGpu& channel, kernel::Process* caller,
                            gpu::Fence dependency = {}) {
        const auto flags = gpu::GpfifoFlags::FenceGet |
            (dependency.id ? gpu::GpfifoFlags::FenceWait : gpu::GpfifoFlags::None);
        std::array<u32, 8> request{0, 0, 1, u32(flags), dependency.id, dependency.value, 0, 0};
        const auto entry = std::bit_cast<gpu::GpfifoEntry>(COMMAND | (5ull << 42));
        std::memcpy(request.data() + 6, &entry, sizeof(entry));
        const auto bytes = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
        nv::ioctl::IoctlContext context{
            .system = system, .process = caller,
            .in_stream = ztd::io::MemoryStream(bytes),
            .out_stream = ztd::io::MemoryStream(bytes)};
        const auto result = channel.ioctl(context, 0x48, 0x08);
        return std::pair(result, gpu::Fence{request[4], request[5]});
    };
    const auto wait = [&](const auto& submission) {
        check(submission.first == nv::NvResult::Success &&
                  system.getGpu().getVideoSyncpoints().waitFor(
                      submission.second.id, submission.second.value, std::chrono::milliseconds(1000)),
              "Bound channel submission reaches its own completion fence");
    };
    const auto before = system.getGpu().getPfifo().getSubmittedCount();
    check(submit(unbound, process).first == nv::NvResult::InvalidState &&
              system.getGpu().getPfifo().getSubmittedCount() == before,
          "Submit without AS binding cannot silently use the process MMU");
    wait(submit(a, process));
    wait(submit(b, process));
    wait(submit(shared, process));
    check(*reinterpret_cast<u32*>(hosts[0] + 0x1000) == 0x1111 &&
              *reinterpret_cast<u32*>(hosts[1] + 0x1000) == 0x2222,
          "PFIFO fetch and semaphore writes resolve through each bound AS");
    auto& processes = system.getOs().getKernel().getProcessManager();
    auto* foreign = processes.createProcess("Foreign AS caller");
    check(bind(*as_a, 99, foreign) == nv::NvResult::BadParameter &&
              submit(a, foreign).first == nv::NvResult::InvalidState &&
              unbound.bindAddressSpace(*foreign, a.getChannelState()->address_space) ==
                  nv::NvResult::BadParameter,
          "Binding, mapping ownership and submit cannot cross CPU processes");
    processes.destroyProcess(foreign);

    auto transient_as = std::make_unique<nv::ioctl::NvHostAsGpu>();
    auto transient = std::make_unique<nv::ioctl::NvHostGpu>();
    fds[5] = transient.get();
    check(bind(*transient_as, 5, process) == nv::NvResult::Success &&
              map(*transient_as, handles[0]) == nv::NvResult::Success,
          "Prepare an AS/channel whose FDs will close with queued work");
    std::weak_ptr<gpu::GMmu> retained_space = transient->getChannelState()->address_space;
    const auto dependency = system.getGpu().getVideoSyncpoints().allocate();
    check(dependency.has_value(), "Allocate lifetime-test dependency");
    const auto pending = submit(*transient, process, {*dependency, 1});
    check(pending.first == nv::NvResult::Success, "Queue work behind an unsignaled fence");
    fds.erase(5);
    transient.reset();
    transient_as.reset();
    check(!retained_space.expired(), "Queued work owns its AS after both device FDs close");
    system.getGpu().getVideoSyncpoints().complete(*dependency, 1);
    wait(pending);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retained_space.expired() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(retained_space.expired(), "Completed work releases a closed AS without a process-wide strong owner");
    as_a.reset();
    as_b.reset();
    wait(submit(a, process));
    wait(submit(b, process));
    check(system.getGpu().freeMap(handles[0]) && system.getGpu().freeMap(handles[1]),
          "Release isolated nvmap handles after completion");

    std::optional<gpu::GMmu> reused;
    std::array<u32, 2> backing{0x1234, 0x5678};
    for (usize i = 0; i < backing.size(); ++i) {
        reused.reset();
        reused.emplace(process->getMmu());
        reused->map(COMMAND, {reinterpret_cast<uptr>(&backing[i]), sizeof(u32)});
        check(reused->load<u32>(COMMAND) == backing[i],
              "Reconstructing an MMU at the same host address cannot revive stale TLS mappings");
    }
    std::cout << "PASS: AS binding, same-VA isolation, ownership, FD lifetime and MMU identity\n";
}

void testGpfifoInput(System& system, kernel::Process* process) {
    namespace nv = services::nvdrv;
    namespace gpu = hw::tegra_x1::gpu;
    class CaptureChannel final : public nv::ioctl::NvHostGpu {
      public:
        u32 calls{0};
        std::vector<gpu::GpfifoEntry> captured;
        nv::NvResult submitGpfifo(System*, kernel::Process*, u64, u32 count,
            nv::ioctl::InOut<gpu::GpfifoFlags, u32> flags,
            nv::ioctl::InOutSingle<gpu::Fence> fence,
            const gpu::GpfifoEntry* entries) override {
            ++calls;
            captured.clear();
            if (count)
                captured.assign(entries, entries + count);
            flags = 0;
            fence = {7, 9};
            return nv::NvResult::Success;
        }
    } channel;
    for (u32 mode = 0; mode < 3; ++mode) {
        std::array<u8, 48> input{}, output{}, inline_data{};
        const std::array<u64, 2> entry_words{0x1234567800000100ull,
                                            0x8765432100000200ull};
        const auto call = [&](u32 count, usize input_size, usize output_size,
                              std::optional<usize> inline_size) {
            input.fill(0);
            output.fill(0xa5);
            std::memcpy(input.data() + 9, &count, 4);
            std::memcpy(input.data() + 25, entry_words.data(), 16);
            std::memcpy(inline_data.data() + 1, entry_words.data(), 16);
            nv::ioctl::IoctlContext context{
                .system = system, .process = process,
                .in_stream = ztd::io::MemoryStream(std::span(input).first(input_size + 1)),
                .out_stream = ztd::io::MemoryStream(std::span(output).first(output_size + 1))};
            context.in_stream->seekBy(1);
            context.out_stream->seekBy(1);
            if (inline_size) {
                context.in_buffer_stream.emplace(std::span(inline_data).first(*inline_size + 1));
                context.in_buffer_stream->seekBy(1);
            }
            const u32 calls_before = channel.calls;
            const auto result = mode == 2
                ? channel.ioctl2(context, 0x48, 0x1b)
                : channel.ioctl(context, 0x48, mode == 0 ? 0x08 : 0x1b);
            if (result != nv::NvResult::Success) {
                check(channel.calls == calls_before, "Invalid GPFIFO input is not submitted");
                check(std::all_of(output.begin(), output.end(),
                    [](u8 value) { return value == 0xa5; }),
                    "Invalid GPFIFO input leaves output untouched");
            }
            return result;
        };
        for (usize size = 0; size < 24; ++size) {
            check(call(2, size, 24, 16) == nv::NvResult::InvalidSize,
                  "Truncated GPFIFO header is rejected");
            check(call(2, 40, size, 16) == nv::NvResult::InvalidSize,
                  "Truncated GPFIFO output is rejected");
        }
        for (usize size = 0; size < 16; ++size)
            check(call(2, mode == 2 ? 24 : 24 + size, 24, size) ==
                      nv::NvResult::InvalidSize,
                  "Truncated GPFIFO entry arrays are rejected");
        check(call(0xffffffffu, 40, 24, 16) == nv::NvResult::InvalidSize,
              "Large GPFIFO counts do not overflow validation");
        if (mode == 2)
            check(call(2, 24, 24, std::nullopt) == nv::NvResult::BadParameter,
                  "Missing inline GPFIFO buffer is rejected");
        check(call(2, mode == 2 ? 24 : 40, 24, 16) == nv::NvResult::Success,
              "Valid GPFIFO submission is accepted");
        check(channel.captured.size() == 2 &&
                  std::memcmp(channel.captured.data(), entry_words.data(), 16) == 0,
              "Unaligned GPFIFO entries are copied exactly");
        gpu::Fence fence{};
        std::memcpy(&fence, output.data() + 17, 8);
        check(output[13] == 0 && fence.id == 7 && fence.value == 9 &&
                  output[0] == 0xa5 && output[25] == 0xa5,
              "GPFIFO response writes only its header and returns the fence");
        check(call(0, 24, 24, std::nullopt) == nv::NvResult::Success &&
                  channel.captured.empty(),
              "Empty GPFIFO submission needs no entry bytes");
    }
    vaddr_t guest;
    const auto host = process->createMemory(kernel::HEAP_REGION, 0x2000,
        kernel::MemoryType::Normal_4_0_0, kernel::MemoryPermission::ReadWrite, guest);
    const std::array<u64, 2> pointed_entries{0x1234567800000100ull,
                                             0x8765432100000200ull};
    std::memcpy(reinterpret_cast<void*>(host + 0xffc), pointed_entries.data(), 16);
    const std::array<u64, 2> appended_entries{0x1111111100000300ull,
                                              0x2222222200000400ull};
    const auto pointer_call = [&](u64 address, u32 nr, bool inline_entries,
                                  usize entry_bytes) {
        std::array<u8, 40> request{};
        std::array<u8, 24> response{};
        const u32 count = 2;
        std::memcpy(request.data(), &address, 8);
        std::memcpy(request.data() + 8, &count, 4);
        std::memcpy(request.data() + 24, appended_entries.data(), 16);
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(request).first(
                inline_entries ? 24 : 24 + entry_bytes)),
            .out_stream = ztd::io::MemoryStream(response)};
        if (inline_entries)
            context.in_buffer_stream.emplace(std::span(request).subspan(24, entry_bytes));
        const auto before = channel.calls;
        const auto result = inline_entries ? channel.ioctl2(context, 0x48, nr)
                                          : channel.ioctl(context, 0x48, nr);
        if (result != nv::NvResult::Success) {
            check(channel.calls == before, "Truncated IPC entries never reach submit");
            check(std::all_of(response.begin(), response.end(),
                             [](u8 value) { return value == 0; }),
                  "Truncated IPC entries leave the response untouched");
        }
        return result;
    };
    for (u32 mode = 0; mode < 3; ++mode) {
        const u32 nr = mode == 0 ? 0x08u : 0x1bu;
        for (u64 address : {u64(0), u64(guest + 0xffc), u64(guest + 0x1ffc),
                            0xfffffffffffffff8ull}) {
            check(pointer_call(address, nr, mode == 2, 16) == nv::NvResult::Success &&
                      channel.captured.size() == 2 &&
                      std::memcmp(channel.captured.data(), appended_entries.data(), 16) == 0,
                  "Switch submit uses IPC entries regardless of the ignored pointer");
            for (usize bytes = 0; bytes < 16; ++bytes)
                check(pointer_call(address, nr, mode == 2, bytes) == nv::NvResult::InvalidSize,
                      "A valid guest pointer cannot rescue truncated IPC entries");
        }
    }
    nv::ioctl::NvHostGpu real_channel;
    bindTestChannel(real_channel, process);
    u32 channel_id = 0;
    for (u32 nr : {0x18u, 0x1au}) {
        std::array<u32, 8> allocation{64, 1, 0, 0, 0, 0, 0, 0};
        std::array<u32, 8> allocated{};
        nv::ioctl::IoctlContext allocation_context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(allocation.data()), sizeof(allocation))),
            .out_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(allocated.data()), sizeof(allocated)))};
        check(real_channel.ioctl(allocation_context, 0x48, nr) == nv::NvResult::Success &&
                  allocated[3] != 0 && allocated[4] == 0 &&
                  (channel_id == 0 || channel_id == allocated[3]),
              "GPFIFO allocation returns the channel's stable initial fence without incrementing it");
        channel_id = allocated[3];
    }
    std::array<u32, 6> empty_request{0, 0, 0,
        static_cast<u32>(gpu::GpfifoFlags::FenceGet), 0, 0};
    std::array<u32, 6> response{};
    nv::ioctl::IoctlContext real_context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(empty_request.data()), sizeof(empty_request))),
        .out_stream = ztd::io::MemoryStream(std::span(
            reinterpret_cast<u8*>(response.data()), sizeof(response)))};
    const auto submitted = system.getGpu().getPfifo().getSubmittedCount();
    auto invalid_request = empty_request;
    invalid_request[3] = static_cast<u32>(gpu::GpfifoFlags::FenceGet |
                                        gpu::GpfifoFlags::FenceWait);
    invalid_request[4] = 191;
    response.fill(0xa5a5a5a5);
    auto invalid_context = real_context;
    invalid_context.in_stream.emplace(std::span(
        reinterpret_cast<u8*>(invalid_request.data()), sizeof(invalid_request)));
    check(real_channel.ioctl(invalid_context, 0x48, 0x1b) == nv::NvResult::BadValue &&
              system.getGpu().getPfifo().getSubmittedCount() == submitted &&
              response[0] == 0xa5a5a5a5,
          "Invalid wait fence is rejected before queuing or writing output");
    invalid_request[3] = static_cast<u32>(gpu::GpfifoFlags::FenceWait |
                                        gpu::GpfifoFlags::IncrementWithValue);
    invalid_request[4] = 0;
    invalid_request[5] = 3;
    invalid_context.in_stream->seekTo(0);
    check(real_channel.ioctl(invalid_context, 0x48, 0x1b) == nv::NvResult::BadParameter &&
              system.getGpu().getPfifo().getSubmittedCount() == submitted &&
              real_channel.getChannelState()->fence_target == 0 &&
              response[0] == 0xa5a5a5a5,
          "Ambiguous wait and increment-count input is rejected without reserving a fence");
    check(real_channel.ioctl(real_context, 0x48, 0x1b) == nv::NvResult::Success &&
              system.getGpu().getPfifo().getSubmittedCount() == submitted + 1,
          "Validated empty submission reaches the real PFIFO queue");
    check(response[4] == channel_id && response[5] == 1 &&
              system.getGpu().getVideoSyncpoints().waitFor(
              response[4], response[5], std::chrono::milliseconds(1000)),
          "Validated submission returns a completion fence");
    std::cout << "PASS: GPFIFO ioctl/ioctl2 sizes, entries, output and empty submissions\n";
}

void testPfifoDecode(System& system, kernel::Process* process) {
    namespace gpu = hw::tegra_x1::gpu;
    auto& device = system.getGpu();
    auto& gmmu = process->getGMmu();
    auto state = std::make_shared<gpu::PfifoDecodeState>();
    std::vector<std::vector<u8>> allocations;
    std::vector<uptr> mappings;
    uptr next_address = 0x2000000;
    const auto map_bytes = [&](uptr address, std::span<const u8> bytes) {
        allocations.emplace_back(bytes.begin(), bytes.end());
        auto& data = allocations.back();
        gmmu.map(address, {reinterpret_cast<uptr>(data.data()), data.size()});
        mappings.push_back(address);
    };
    const auto map_words = [&](std::span<const u32> words) {
        const auto address = next_address;
        next_address += 0x1000;
        map_bytes(address, {reinterpret_cast<const u8*>(words.data()), words.size_bytes()});
        return address;
    };
    const auto entry = [](uptr address, u32 words) {
        return std::bit_cast<gpu::GpfifoEntry>(
            static_cast<u64>(address) | (static_cast<u64>(words) << 42));
    };
    const auto header = [](u32 opcode, u32 method, u32 count) {
        return (opcode << 29) | (count << 16) | method;
    };
    const auto run = [&](std::initializer_list<gpu::GpfifoEntry> entries,
                         const std::shared_ptr<gpu::PfifoDecodeState>& decode,
                         bool expect_success = true) {
        std::promise<void> finished;
        auto done = finished.get_future();
        device.getPfifo().submitEntries(gmmu, {entries.begin(), entries.size()},
            gpu::GpfifoFlags::None, [&] { finished.set_value(); }, {}, decode);
        if (expect_success) {
            check(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                  "PFIFO decoder submission completes");
        } else {
            check(device.getPfifo().waitForCompletion(device.getPfifo().getSubmittedCount()) &&
                      decode->failed && done.wait_for(std::chrono::milliseconds(0)) !=
                          std::future_status::ready,
                  "Faulted packet retires without publishing success");
        }
    };
    const std::array<u32, 2> bind{header(1, 0, 1), 0xb197};
    run({entry(map_words(bind), 2)}, state);
    auto& engine = state->context->three_d;

    // Bytes after the first entry are mapped but are not part of its payload.
    const std::array<u32, 3> first{header(1, 0xd00, 2), 0x1111, 0xdeadbeef};
    const std::array<u32, 1> last{0x2222};
    const auto first_address = map_words(first);
    const auto last_address = map_words(last);
    run({entry(first_address, 2), entry(last_address, 1)}, state);
    check(engine.getReg(0xd00) == 0x1111 && engine.getReg(0xd01) == 0x2222,
          "Packet continues at the next entry without reading past the first");

    // Every byte boundary, including inside a header and inside a payload word.
    std::array<u32, 3> split{header(1, 0xd02, 2), 0xaabbccdd, 0x12345678};
    const auto split_bytes = std::span(reinterpret_cast<const u8*>(split.data()), sizeof(split));
    for (usize cut = 1; cut < sizeof(split); ++cut) {
        split[1] = 0xaabbcc00u + static_cast<u32>(cut);
        split[2] = 0x12345600u + static_cast<u32>(cut);
        const auto address = next_address;
        next_address += 0x1000;
        map_bytes(address, split_bytes.first(cut));
        map_bytes(address + cut, split_bytes.subspan(cut));
        run({entry(address, 3)}, state);
        check(engine.getReg(0xd02) == split[1] && engine.getReg(0xd03) == split[2],
              "Header and payload words span noncontiguous host allocations");
    }

    // Subdevice masks (clb06f.h, dev_pbdma.ref): the channel is subdevice 1.
    // While inactive, methods are consumed but not delivered and conditional
    // segments are skipped.
    const auto set_mask = [](u32 mask) { return (1u << 16) | (mask << 4); };
    const auto store_mask = [](u32 mask) { return (2u << 16) | (mask << 4); };
    constexpr u32 USE_MASK = 3u << 16;
    const auto conditional = [&](uptr address, u32 words) {
        return std::bit_cast<gpu::GpfifoEntry>(
            static_cast<u64>(address | 1u) | (static_cast<u64>(words) << 42));
    };
    const std::array<u32, 4> inactive{set_mask(0x2), header(1, 0xd20, 2), 0x51, 0x52};
    const std::array<u32, 2> skipped_cond{header(1, 0xd22, 1), 0x53};
    const std::array<u32, 2> inactive_plain{header(1, 0xd25, 1), 0x56};
    const std::array<u32, 5> reactivate{store_mask(0x1), USE_MASK, header(1, 0xd23, 1), 0x54,
                                        0};
    const std::array<u32, 2> run_cond{header(1, 0xd24, 1), 0x55};
    run({entry(map_words(inactive), 4), conditional(map_words(skipped_cond), 2),
         entry(map_words(inactive_plain), 2), entry(map_words(reactivate), 4),
         conditional(map_words(run_cond), 2)}, state);
    check(engine.getReg(0xd20) == 0 && engine.getReg(0xd21) == 0 &&
              engine.getReg(0xd22) == 0 && engine.getReg(0xd25) == 0,
          "Inactive subdevice drops methods and skips conditional segments");
    check(engine.getReg(0xd23) == 0x54 && engine.getReg(0xd24) == 0x55,
          "STORE then USE reactivates the subdevice; conditional segments fetch");
    const std::array<u32, 6> store_inactive{store_mask(0x2), USE_MASK, header(1, 0xd26, 1), 0x57,
                                            set_mask(0x1), header(1, 0xd27, 1)};
    const std::array<u32, 1> tail{0x58};
    run({entry(map_words(store_inactive), 6), entry(map_words(tail), 1)}, state);
    check(engine.getReg(0xd26) == 0 && engine.getReg(0xd27) == 0x58,
          "A stored mask excluding the subdevice deactivates it; SET restores delivery");

    // SEMAPHORED reductions and a 16-byte report whose halves live in separate
    // host mappings (clb06f.h SEMAPHORED_OPERATION/REDUCTION/FORMAT).
    constexpr uptr REPORT = 0x5200000;
    const std::array<u32, 2> report_head{10, 0};
    const std::array<u32, 2> report_tail{0, 0};
    map_bytes(REPORT, {reinterpret_cast<const u8*>(report_head.data()), 8});
    map_bytes(REPORT + 8, {reinterpret_cast<const u8*>(report_tail.data()), 8});
    const auto word_at = [&](u64 offset) {
        u32 value;
        const auto bytes = gmmu.readMappedBytes(REPORT + offset, 4);
        std::memcpy(&value, bytes.data(), 4);
        return value;
    };
    const auto trigger = [&](u32 payload, u32 arg) {
        const std::array<u32, 5> words{header(1, 4, 3), 0, static_cast<u32>(REPORT), payload,
                                       0};
        const std::array<u32, 2> fire{header(1, 7, 1), arg};
        run({entry(map_words(std::span(words).first(4)), 4), entry(map_words(fire), 2)}, state);
    };
    constexpr u32 NO_WFI = 1u << 20, UNSIGNED = 1u << 31;
    const auto reduction = [](u32 op) { return 0x10u | (op << 27) | NO_WFI; };
    trigger(0x77, 0x2 | NO_WFI); // 16-byte release
    const auto timestamp = word_at(8);
    check(word_at(0) == 0x77 && (timestamp != 0 || word_at(12) != 0),
          "A 16-byte release spanning two host mappings writes payload and timestamp");
    trigger(5, reduction(5)); // ADD
    check(word_at(0) == 0x7c, "Semaphore reduction ADD");
    trigger(0xffffffff, reduction(1)); // MAX, signed: -1 < 0x7c
    check(word_at(0) == 0x7c, "Signed MAX treats 0xffffffff as -1");
    trigger(0xffffffff, reduction(1) | UNSIGNED);
    check(word_at(0) == 0xffffffff, "Unsigned MAX treats 0xffffffff as large");
    trigger(0xf0, reduction(3)); // AND
    check(word_at(0) == 0xf0, "Semaphore reduction AND");
    trigger(0x0f, reduction(4)); // OR
    trigger(0xff, reduction(2)); // XOR
    check(word_at(0) == 0, "Semaphore reduction OR then XOR");
    trigger(3, reduction(0)); // MIN signed: min(0, 3)
    check(word_at(0) == 0, "Semaphore reduction MIN");
    {
        auto faulted = std::make_shared<gpu::PfifoDecodeState>();
        const std::array<u32, 4> words{header(1, 4, 3), 0, static_cast<u32>(REPORT), 1};
        const std::array<u32, 2> fire{header(1, 7, 1), reduction(6)};
        run({entry(map_words(words), 4), entry(map_words(fire), 2)}, faulted, false);
        check(faulted->error == 25 && word_at(0) == 0,
              "Reduction INC/DEC is rejected rather than guessed");
    }

    // Host memory operations (MEM_OP_A..D) and flushes are accepted without
    // faulting the channel
    {
        const std::array<u32, 8> mem_ops{header(1, 8, 1), 0x1234, header(1, 9, 1), 0x5678,
                                         header(1, 0xb, 1), 0x0, header(1, 0xd, 1), 9u << 27};
        const std::array<u32, 2> membar{header(1, 0xd, 1), 5u << 27};
        run({entry(map_words(mem_ops), 8), entry(map_words(membar), 2)}, state);
        check(!state->failed, "Interrupt, flush and MEM_OP methods do not fault the channel");
    }

    // A destroyed channel state gives its fence counter back; the next owner
    // continues from the counter's value instead of an already reached target.
    {
        gpu::Fence initial_fence{}, reused{};
        u32 id;
        {
            auto temp = std::make_shared<gpu::PfifoDecodeState>();
            check(device.reserveFence(initial_fence, temp.get(), 2) && device.isFenceSyncpoint(initial_fence.id),
                  "Reserve a channel fence counter");
            id = initial_fence.id;
            check(device.getVideoSyncpoints().complete(id, 2).value() >= 2,
                  "Advance the channel's fence counter");
        }
        check(!device.isFenceSyncpoint(id), "A destroyed channel no longer owns its fence counter");
        auto next = std::make_shared<gpu::PfifoDecodeState>();
        check(device.reserveFence(reused, next.get(), 1) && reused.id == id &&
                  reused.value == device.getVideoSyncpoints().read(id).value() + 1,
              "A reused fence counter continues from its current value");
    }

    // Each decode stream owns its pending method count, including ONE_INC.
    const std::array<u32, 2> one_inc{header(5, 0xd04, 3), 0x41};
    run({entry(map_words(one_inc), 2)}, state);
    check(state->remaining == 2 && engine.getReg(0xd04) == 0x41,
          "Incomplete packet is retained across submissions");
    auto other = std::make_shared<gpu::PfifoDecodeState>();
    run({entry(map_words(bind), 2)}, other);
    const std::array<u32, 1> immediate{header(4, 0xd06, 0x66)};
    run({entry(map_words(immediate), 1)}, other);
    const std::array<u32, 2> rest{0x51, 0x52};
    run({entry(map_words(rest), 2)}, state);
    check(state->remaining == 0 && engine.getReg(0xd05) == 0x52 &&
              other->context->three_d.getReg(0xd06) == 0x66 && engine.getReg(0xd06) == 0,
          "ONE_INC resumes without consuming another channel's header");

    const std::array<u32, 4> noninc{header(3, 0xd07, 3), 1, 2, 3};
    run({entry(map_words(noninc), 4)}, state);
    check(engine.getReg(0xd07) == 3, "Nonincrementing packet retains its method");
    const std::array<u32, 3> legacy{(2u << 18) | (0x500u << 2), 0x71, 0x72};
    run({entry(map_words(legacy), 3)}, state);
    check(engine.getReg(0x500) == 0x71 && engine.getReg(0x501) == 0x72,
          "Legacy incrementing header uses byte-addressed method fields");
    const std::array<u32, 3> legacy_noninc{
        (2u << 29) | (2u << 18) | (0x502u << 2), 0x81, 0x82};
    run({entry(map_words(legacy_noninc), 3)}, state);
    check(engine.getReg(0x502) == 0x82,
          "Legacy nonincrementing header retains its method");

    // A macro must receive the complete burst before it executes.
    const std::array<u32, 6> macro_program{
        header(4, 0x45, 0), header(4, 0x47, 0), header(4, 0x48, 0),
        header(3, 0x46, 2),
        1u | (6u << 4) | (1u << 7) | (0xd0au << 14),
        1u | (1u << 4)};
    run({entry(map_words(macro_program), 6)}, state);
    const std::array<u32, 2> macro_first{header(5, 0xe00, 2), 0x123};
    run({entry(map_words(macro_first), 2)}, state);
    check(engine.getReg(0xd0a) == 0 && state->remaining == 1,
          "Partial macro burst does not execute");
    run({entry(map_words(immediate), 1)}, other);
    const std::array<u32, 1> macro_last{0x456};
    run({entry(map_words(macro_last), 1)}, state);
    check(engine.getReg(0xd0a) == 0x456 && state->remaining == 0,
          "Macro executes with all arguments after the next submission");

    // A mapping hole must not crash the worker or dispatch a partial word.
    const std::array<u32, 2> hole{header(1, 0xd08, 1), 0xffffffff};
    const auto address = next_address;
    next_address += 0x1000;
    map_bytes(address, {reinterpret_cast<const u8*>(hole.data()), 6});
    run({entry(address, 2)}, state, false);
    check(state->remaining == 0 && engine.getReg(0xd08) == 0,
          "Unmapped tail discards a partial word and resets pending state");
    auto recovered = std::make_shared<gpu::PfifoDecodeState>();
    run({entry(map_words(bind), 2)}, recovered);
    auto& recovered_engine = recovered->context->three_d;
    const std::array<u32, 1> recovery{header(4, 0xd08, 0x99)};
    run({entry(map_words(recovery), 1)}, recovered);
    check(recovered_engine.getReg(0xd08) == 0x99, "New channel remains usable after another channel faults");
    run({entry(next_address, 1)}, recovered, false);
    auto ended = std::make_shared<gpu::PfifoDecodeState>();
    run({entry(map_words(bind), 2)}, ended);
    run({entry(map_words(recovery), 1)}, ended);
    const std::array<u32, 2> segment_end{header(7, 0, 0), header(4, 0xd08, 0x55)};
    run({entry(map_words(segment_end), 2)}, ended);
    check(ended->context->three_d.getReg(0xd08) == 0x99, "END_PB_SEGMENT skips the remaining entry words");
    // Exercise the FD-to-PFIFO wiring, with in-place ioctl input/output.
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostGpu channel_a, channel_b;
    bindTestChannel(channel_a, process);
    bindTestChannel(channel_b, process);
    const auto submit_channel = [&](nv::ioctl::NvHostGpu& channel,
                                    std::span<const u32> words,
                                   gpu::GpfifoFlags flags = gpu::GpfifoFlags::FenceGet,
                                   bool inline_entries = false) {
        std::array<u32, 8> request{0, 0, 1,
            static_cast<u32>(flags), 0, 0, 0, 0};
        const auto command = entry(map_words(words), static_cast<u32>(words.size()));
        std::memcpy(request.data() + 6, &command, sizeof(command));
        const auto bytes = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(inline_entries ? bytes.first(24) : bytes),
            .in_buffer_stream = inline_entries
                ? std::optional(ztd::io::MemoryStream(bytes.subspan(24))) : std::nullopt,
            .out_stream = ztd::io::MemoryStream(bytes)};
        const auto result = inline_entries ? channel.ioctl2(context, 0x48, 0x1b)
                                           : channel.ioctl(context, 0x48, 0x1b);
        check(result == nv::NvResult::Success, "Channel submission accepts supported entry formats");
        if (any(flags & gpu::GpfifoFlags::FenceGet))
            check(device.getVideoSyncpoints().waitFor(request[4], request[5],
                                                      std::chrono::milliseconds(1000)),
                  "Channel submission reaches its completion fence");
        else
            check(request[4] == 0 && request[5] == 0 &&
                      device.getPfifo().waitForCompletion(device.getPfifo().getSubmittedCount()),
                  "Native submission without FenceGet executes without a fabricated fence");
    };
    const std::array<u32, 2> channel_first{header(1, 0xd0b, 2), 0xab};
    const std::array<u32, 1> channel_other{header(4, 0xd0d, 0xcd)};
    const std::array<u32, 1> channel_last{0xbc};
    submit_channel(channel_a, bind);
    submit_channel(channel_b, bind);
    submit_channel(channel_a, channel_first);
    submit_channel(channel_b, channel_other);
    submit_channel(channel_a, channel_last);
    const auto& a_engine = channel_a.getChannelState()->context->three_d;
    const auto& b_engine = channel_b.getChannelState()->context->three_d;
    check(a_engine.getReg(0xd0b) == 0xab && a_engine.getReg(0xd0c) == 0xbc &&
              a_engine.getReg(0xd0d) == 0 && b_engine.getReg(0xd0d) == 0xcd &&
              b_engine.getReg(0xd0b) == 0,
          "Actual nvdrv channels keep independent pending packet counts");
    const std::array<u32, 1> native_first{header(4, 0xd0b, 0x123)};
    const std::array<u32, 1> native_second{header(4, 0xd0c, 0x234)};
    submit_channel(channel_a, native_first,
                   gpu::GpfifoFlags::HwFormat | gpu::GpfifoFlags::FenceGet);
    submit_channel(channel_a, native_second, gpu::GpfifoFlags::HwFormat, true);
    check(a_engine.getReg(0xd0b) == 0x123 && a_engine.getReg(0xd0c) == 0x234,
          "Ioctl1 and inline ioctl2 execute native HW_FORMAT words including flags 0x4");
    auto level_entry = entry(map_words(channel_first), 2);
    level_entry.level_subroutine = 1;
    auto level_tail = entry(map_words(channel_last), 1);
    run({level_entry}, ended);
    check(ended->remaining == 1, "Subroutine-level entry retains a partial packet");
    run({level_tail}, ended);
    check(ended->context->three_d.getReg(0xd0b) == 0xab &&
              ended->context->three_d.getReg(0xd0c) == 0xbc && !ended->failed,
          "Packet arguments continue from subroutine level into main level");

    nv::ioctl::NvHostGpu guest_count_channel;
    bindTestChannel(guest_count_channel, process);
    gpu::Fence initial_count{};
    check(device.reserveFence(initial_count, guest_count_channel.getChannelState().get(), 0),
          "Allocate a guest-count channel syncpoint");
    alignas(8) std::array<u32, 1> count_gate{};
    const auto count_gate_address = next_address;
    next_address += 0x1000;
    gmmu.map(count_gate_address, {reinterpret_cast<uptr>(count_gate.data()), sizeof(count_gate)});
    mappings.push_back(count_gate_address);
    auto submit_guest_count = [&](std::span<const u32> words, u32 increments,
                                  gpu::GpfifoFlags flags) {
        auto command = entry(map_words(words), static_cast<u32>(words.size()));
        command.level_subroutine = 1;
        std::array<u32, 6> request{0, 0, 1, static_cast<u32>(flags), 0, increments};
        const auto bytes = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(bytes),
            .in_buffer_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(&command), sizeof(command))),
            .out_stream = ztd::io::MemoryStream(bytes)};
        check(guest_count_channel.ioctl2(context, 0x48, 0x1b) == nv::NvResult::Success,
              "Ioctl2 accepts native entries with guest increment reservations");
        return gpu::Fence{request[4], request[5]};
    };
    const u32 count_operation = initial_count.id;
    const std::array<u32, 13> guest_count_commands{
        header(1, 0, 1), 0xb197,
        header(1, 4, 4), 0, static_cast<u32>(count_gate_address), 1, 1,
        header(1, 0xb2, 1), count_operation,
        header(1, 0xb2, 1), count_operation | (1u << 16),
        header(1, 0xb2, 1), count_operation | (1u << 20)};
    const auto counted = submit_guest_count(guest_count_commands, 3,
        gpu::GpfifoFlags::HwFormat | gpu::GpfifoFlags::IncrementWithValue);
    check(counted.id == initial_count.id && counted.value == 3 &&
              !device.getVideoSyncpoints().waitFor(counted.id, counted.value,
                                                    std::chrono::milliseconds(20)) &&
              device.getVideoSyncpoints().read(counted.id) == 0,
          "Flags 0x104 reserve the supplied count without signaling pending work");
    std::atomic_ref<u32>(count_gate[0]).store(1, std::memory_order_release);
    check(device.getVideoSyncpoints().waitFor(counted.id, counted.value,
                                               std::chrono::milliseconds(1000)) &&
              device.getPfifo().waitForCompletion(device.getPfifo().getSubmittedCount()) &&
              device.getVideoSyncpoints().read(counted.id) == 3,
          "Guest syncpoint commands complete reservations without a duplicate host increment");
    const std::array<u32, 4> more_counts{
        header(1, 0xb2, 1), count_operation,
        header(1, 0xb2, 1), count_operation | (1u << 16) | (1u << 20)};
    const auto combined = submit_guest_count(more_counts, 2,
        gpu::GpfifoFlags::HwFormat | gpu::GpfifoFlags::IncrementWithValue |
        gpu::GpfifoFlags::FenceGet);
    check(combined.id == counted.id && combined.value == 6 &&
              device.getVideoSyncpoints().waitFor(combined.id, combined.value,
                                                   std::chrono::milliseconds(1000)) &&
              device.getVideoSyncpoints().read(combined.id) == 6,
          "Guest reservation and terminal host fence accumulate on the same channel counter");
    auto invalid_increment = std::make_shared<gpu::PfifoDecodeState>();
    run({entry(map_words(bind), 2)}, invalid_increment);
    const std::array<u32, 2> invalid_count{header(1, 0xb2, 1), 191};
    run({entry(map_words(invalid_count), 2)}, invalid_increment, false);
    check(invalid_increment->error == 25,
          "3D increment of an unallocated syncpoint faults without publishing success");
    for (const auto base : mappings)
        gmmu.unmap(base);
    std::cout << "PASS: PFIFO packet bounds, split mappings, continuation and decoder modes\n";
}

void testChannelScheduling(System& system, kernel::Process* process) {
    namespace gpu = hw::tegra_x1::gpu;
    auto& device = system.getGpu();
    auto& gmmu = process->getGMmu();
    auto a = std::make_shared<gpu::PfifoDecodeState>();
    auto b = std::make_shared<gpu::PfifoDecodeState>();
    std::vector<std::vector<u32>> commands;
    std::vector<uptr> mappings;
    uptr next = 0x5000000;
    const auto immediate = [](u32 method, u32 value) {
        return (4u << 29) | (value << 16) | method;
    };
    struct Job {
        std::future<void> done;
        gpu::Fence fence;
        u64 serial;
    };
    const auto enqueue = [&](const std::shared_ptr<gpu::PfifoDecodeState>& state,
                             std::vector<u32> words, gpu::Fence wait = {}) {
        commands.push_back(std::move(words));
        auto& bytes = commands.back();
        const auto address = next;
        next += 0x1000;
        gmmu.map(address, {reinterpret_cast<uptr>(bytes.data()), bytes.size() * 4});
        mappings.push_back(address);
        const auto entry = std::bit_cast<gpu::GpfifoEntry>(
            static_cast<u64>(address) | (static_cast<u64>(bytes.size()) << 42));
        gpu::Fence fence{};
        check(device.reserveFence(fence, state.get()), "Channel reserves its fence");
        auto finished = std::make_shared<std::promise<void>>();
        auto done = finished->get_future();
        device.getPfifo().submitEntries(gmmu, {&entry, 1}, wait.id
            ? gpu::GpfifoFlags::FenceWait : gpu::GpfifoFlags::None,
            [finished, signal = device.makeFenceSignal(fence)] {
                signal();
                finished->set_value();
            }, wait, state);
        return Job{std::move(done), fence, device.getPfifo().getSubmittedCount()};
    };
    const auto finish = [&](Job& job) {
        check(job.done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
              "Scheduled channel completes");
    };
    const auto dependency = device.getVideoSyncpoints().allocate();
    check(dependency.has_value(), "External dependency allocation");
    auto first = enqueue(a, {(1u << 29) | (1u << 16), 0xb197, immediate(0xd00, 0x11)},
                         {*dependency, 1});
    auto following = enqueue(a, {immediate(0xd00, 0x22)});
    auto producer = enqueue(b, {(1u << 29) | (1u << 16), 0xb197, immediate(0xd00, 0x33)});
    finish(producer);
    check(first.fence.id != producer.fence.id &&
              !device.getVideoSyncpoints().waitFor(first.fence.id, first.fence.value,
                                                   std::chrono::milliseconds(10)),
          "Another channel's completion cannot signal the blocked channel's fence");
    check(first.done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
              following.done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
              !device.getPfifo().waitForCompletion(producer.serial),
          "Same-channel FIFO and contiguous retirement survive out-of-order execution");
    device.getVideoSyncpoints().complete(*dependency, 1);
    finish(first);
    finish(following);
    check(device.getPfifo().waitForCompletion(producer.serial) &&
              a->context->three_d.getReg(0xd00) == 0x22 &&
              b->context->three_d.getReg(0xd00) == 0x33,
          "Channels retain independent 3D state and retire only after earlier work");

    alignas(16) std::array<u32, 4> semaphore{};
    constexpr uptr SEMAPHORE = 0x5100000;
    gmmu.map(SEMAPHORE, {reinterpret_cast<uptr>(semaphore.data()), sizeof(semaphore)});
    mappings.push_back(SEMAPHORE);
    auto waiting = enqueue(a, {
        (1u << 29) | (3u << 16) | 4u, 0, static_cast<u32>(SEMAPHORE), 1,
        immediate(7, 1), immediate(0x1b, 0x77), immediate(0xd01, 0x44)});
    check(waiting.done.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready,
          "Semaphore acquire yields without completing its submission");
    auto same_channel = enqueue(a, {immediate(0xd01, 0x55)});
    auto releasing = enqueue(b, {
        (1u << 29) | (2u << 16) | 4u, 0, static_cast<u32>(SEMAPHORE + 4),
        immediate(0x1b, 0x66),
        (1u << 29) | (1u << 16) | 5u, static_cast<u32>(SEMAPHORE),
        immediate(0x1b, 1),
        (1u << 29) | (1u << 16) | 5u, static_cast<u32>(SEMAPHORE + 8)});
    finish(releasing);
    finish(waiting);
    finish(same_channel);
    check(semaphore[0] == 0x77 && semaphore[1] == 0x66 && semaphore[2] == 0 &&
              a->context->three_d.getReg(0xd01) == 0x55 &&
              b->context->three_d.getReg(0xd01) == 0,
          "Semaphore resumes at the saved word with its own puller registers");

    const auto syncpoint = device.getVideoSyncpoints().allocate();
    check(syncpoint.has_value(), "Command syncpoint allocation");
    auto sync_wait = enqueue(a, {immediate(0x1c, 1),
        immediate(0x1d, (*syncpoint << 8) | 0x10), immediate(0xd03, 0x45)});
    check(sync_wait.done.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready,
          "SYNCPOINT_B wait yields until its captured threshold is reached");
    auto sync_release = enqueue(b, {immediate(0x1c, 9),
        immediate(0x1d, (*syncpoint << 8) | 1)});
    finish(sync_release);
    finish(sync_wait);
    check(a->context->three_d.getReg(0xd03) == 0x45 &&
              device.getVideoSyncpoints().read(*syncpoint) == 1,
          "Another channel can increment the syncpoint without changing the wait payload");

    // Binding a different engine on B must leave A's subchannel untouched.
    auto copy_binding = enqueue(b, {(1u << 29) | (1u << 16), 0xb0b5, immediate(0x40, 0x12)});
    finish(copy_binding);
    auto resume_3d = enqueue(a, {immediate(0xd02, 0x34)});
    finish(resume_3d);
    check(a->context->three_d.getReg(0xd02) == 0x34 &&
              b->context->copy.getReg(0x40) == 0x12,
          "Subchannel engine bindings are isolated across channel switches");
    for (const auto address : mappings)
        gmmu.unmap(address);
    std::cout << "PASS: cross-channel dependencies, engine state, semaphore resume and fences\n";
}

void testChannelFaults(System& system, kernel::Process* process) {
    namespace gpu = hw::tegra_x1::gpu;
    namespace nv = services::nvdrv;
    nv::ioctl::NvHostGpu channel;
    bindTestChannel(channel, process);
    auto configure_notifier = [&](u32 enable, usize input_size = 24) {
        std::array<u8, 24> bytes{};
        std::memcpy(bytes.data() + 16, &enable, 4);
        nv::ioctl::IoctlContext registration{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(bytes).first(input_size))};
        return channel.ioctl(registration, 0x48, 0x0c);
    };
    for (usize size = 0; size < 24; ++size)
        check(configure_notifier(1, size) == nv::NvResult::InvalidSize,
              "Truncated notifier configuration is rejected");
    check(configure_notifier(1) == nv::NvResult::Success &&
              configure_notifier(0) == nv::NvResult::Success &&
              configure_notifier(1) == nv::NvResult::Success,
          "Switch notifier accepts enable/disable without nvmap handles or storage");
    kernel::Event* event = nullptr;
    check(channel.queryEvent(3, event) == nv::NvResult::Success && event,
          "Channel exposes its error event");
    auto notified = std::make_shared<std::promise<void>>();
    auto notification = notified->get_future();
    event->addSignalCallback([notified] { notified->set_value(); });
    constexpr uptr UNMAPPED = 0x6100000;
    const auto entry = std::bit_cast<gpu::GpfifoEntry>(
        static_cast<u64>(UNMAPPED) | (1ull << 42));
    std::array<u32, 8> request{0, 0, 1,
        static_cast<u32>(gpu::GpfifoFlags::FenceGet), 0, 0, 0, 0};
    std::memcpy(request.data() + 6, &entry, sizeof(entry));
    const auto input = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
    nv::ioctl::IoctlContext context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(input),
        .out_stream = ztd::io::MemoryStream(input)};
    check(channel.ioctl(context, 0x48, 0x1b) == nv::NvResult::Success,
          "Well-formed submission reaches asynchronous validation");
    check(notification.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              !system.getGpu().getVideoSyncpoints().waitFor(request[4], request[5],
                                                           std::chrono::milliseconds(20)),
          "Unmapped command signals an error without signaling successful completion");
    alignas(8) std::array<u8, 16> report{};
    nv::ioctl::IoctlContext query{
        .system = system, .process = process,
        .out_stream = ztd::io::MemoryStream(report)};
    check(channel.ioctl(query, 0x48, 0x17) == nv::NvResult::Success,
          "Channel error notification is readable");
    for (usize size = 0; size < report.size(); ++size) {
        std::array<u8, 16> short_output{};
        short_output.fill(0xa5);
        nv::ioctl::IoctlContext short_query{
            .system = system, .process = process,
            .out_stream = ztd::io::MemoryStream(std::span(short_output).first(size))};
        check(channel.ioctl(short_query, 0x48, 0x17) == nv::NvResult::InvalidSize &&
                  std::all_of(short_output.begin(), short_output.end(),
                              [](u8 value) { return value == 0xa5; }),
              "Truncated error notification output is rejected without writes");
    }
    u64 timestamp;
    u32 info;
    u16 status;
    std::memcpy(&timestamp, report.data(), 8);
    std::memcpy(&info, report.data() + 8, 4);
    std::memcpy(&status, report.data() + 14, 2);
    check(timestamp != 0 && info == 31 && status == 0xffff,
          "MMU fault reports the documented notifier layout and error code");
    {
        auto transient = std::make_unique<nv::ioctl::NvHostGpu>();
        std::array<u64, 3> request_bytes{0, 0, 1};
        nv::ioctl::IoctlContext registration{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(std::span(
                reinterpret_cast<u8*>(request_bytes.data()), sizeof(request_bytes)))};
        check(transient->ioctl(registration, 0x48, 0x0c) == nv::NvResult::Success,
              "Configure notifier using the libnx request layout");
        kernel::Event* late_event = nullptr;
        check(transient->queryEvent(3, late_event) == nv::NvResult::Success && late_event,
              "Query notifier before FD closure");
        late_event->retain();
        auto retained = transient->getChannelState();
        transient.reset();
        retained->fail(31);
        check(late_event->clear(), "Queued fault can signal its retained event after FD closure");
        late_event->release();
    }
    {
        nv::ioctl::NvHostGpu disabled;
        kernel::Event* disabled_event = nullptr;
        disabled.queryEvent(3, disabled_event);
        disabled.getChannelState()->fail(31);
        check(!disabled_event->clear(), "Disabled notifier does not signal an event");
    }

    context.in_stream->seekTo(0);
    context.out_stream->seekTo(0);
    check(channel.ioctl(context, 0x48, 0x1b) == nv::NvResult::InvalidState,
          "Faulted channel rejects further submissions");
    nv::ioctl::NvHostGpu unsupported;
    bindTestChannel(unsupported, process);
    std::array<u32, 6> unsupported_request{0, 0, 0,
        static_cast<u32>(gpu::GpfifoFlags::SyncFence), 0, 0};
    const auto unsupported_bytes = std::span(
        reinterpret_cast<u8*>(unsupported_request.data()), sizeof(unsupported_request));
    nv::ioctl::IoctlContext unsupported_context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(unsupported_bytes),
        .out_stream = ztd::io::MemoryStream(unsupported_bytes)};
    const auto submitted = system.getGpu().getPfifo().getSubmittedCount();
    check(unsupported.ioctl(unsupported_context, 0x48, 0x1b) == nv::NvResult::NotSupported &&
              system.getGpu().getPfifo().getSubmittedCount() == submitted,
          "Unsupported fence format is rejected before queuing");

    auto state = std::make_shared<gpu::PfifoDecodeState>();
    auto faulted = std::make_shared<std::promise<void>>();
    auto fault_ready = faulted->get_future();
    state->on_fault = [faulted] { faulted->set_value(); };
    const auto conditional = std::bit_cast<gpu::GpfifoEntry>(
        static_cast<u64>(UNMAPPED | 1) | (1ull << 42));
    system.getGpu().getPfifo().submitEntries(process->getGMmu(), {&conditional, 1},
        gpu::GpfifoFlags::None, {}, {}, state);
    check(fault_ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              state->error == 31,
          "A conditional entry is fetched while the subdevice is active (unmapped fetch faults)");
    auto inactive_state = std::make_shared<gpu::PfifoDecodeState>();
    inactive_state->subdevice_active = false;
    std::promise<void> skipped;
    auto skipped_ready = skipped.get_future();
    system.getGpu().getPfifo().submitEntries(process->getGMmu(), {&conditional, 1},
        gpu::GpfifoFlags::None, [&] { skipped.set_value(); }, {}, inactive_state);
    check(skipped_ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              !inactive_state->failed,
          "A conditional entry is skipped as a NOP while the subdevice is inactive");
    while (system.getGpu().getVideoSyncpoints().allocate()) {}
    nv::ioctl::NvHostGpu exhausted;
    bindTestChannel(exhausted, process);
    std::array<u32, 6> exhausted_request{0, 0, 0,
        static_cast<u32>(gpu::GpfifoFlags::FenceGet), 0, 0};
    const auto exhausted_bytes = std::span(
        reinterpret_cast<u8*>(exhausted_request.data()), sizeof(exhausted_request));
    nv::ioctl::IoctlContext exhausted_context{
        .system = system, .process = process,
        .in_stream = ztd::io::MemoryStream(exhausted_bytes),
        .out_stream = ztd::io::MemoryStream(exhausted_bytes)};
    const auto before_exhausted = system.getGpu().getPfifo().getSubmittedCount();
    check(exhausted.ioctl(exhausted_context, 0x48, 0x1b) == nv::NvResult::InsufficientMemory &&
              system.getGpu().getPfifo().getSubmittedCount() == before_exhausted,
          "Syncpoint exhaustion cannot return a successful zero fence");
    std::cout << "PASS: channel fault event, notification and no false success fence\n";
}

void testGpuProcessCancellation(System& system) {
    namespace gpu = hw::tegra_x1::gpu;
    using gpu::renderer::metal::stackAutoRelease;
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    auto& device = system.getGpu();
    auto& processes = system.getOs().getKernel().getProcessManager();
    auto* process = processes.createProcess("Queued GPU process");
    auto blocked = std::make_shared<gpu::PfifoDecodeState>();
    const auto dependency = device.getVideoSyncpoints().allocate();
    check(dependency.has_value(), "Cancellation fence allocation");
    std::atomic<bool> executed{false};
    device.getPfifo().submitEntries(process->getGMmu(), {}, gpu::GpfifoFlags::FenceWait,
        [&] { executed = true; }, {*dependency, 1}, blocked);
    std::array<std::shared_ptr<gpu::GMmu>, 2> spaces{
        process->createGpuAddressSpace(), process->createGpuAddressSpace()};
    for (const auto& space : spaces) {
        auto pending = std::make_shared<gpu::PfifoDecodeState>();
        pending->address_space = space;
        check(device.getPfifo().submitEntries(*space, {}, gpu::GpfifoFlags::FenceWait,
            [&] { executed = true; }, {*dependency, 1}, pending),
            "Queue blocked work in an independently owned process AS");
    }
    const auto serial = device.getPfifo().getSubmittedCount();

    auto& renderer = static_cast<gpu::renderer::metal::Renderer&>(device.getRenderer());
    NS_STACK_SCOPED auto gate = renderer.getDevice()->newSharedEvent();
    NS_STACK_SCOPED auto memory = renderer.getDevice()->newBuffer(
        4096, MTL::ResourceStorageModeShared);
    check(gate && memory, "Cancellation GPU resources");
    std::memset(memory->contents(), 0, 4096);
    std::unique_ptr<gpu::renderer::ICommandBuffer> commands(renderer.createCommandBuffer());
    auto* native = static_cast<gpu::renderer::metal::CommandBuffer*>(commands.get());
    native->getCommandBuffer()->encodeWait(gate, 1);
    native->getBlitCommandEncoder()->fillBuffer(memory, NS::Range(0, 16), 0x7f);
    commands.reset();
    auto destroyed = std::async(std::launch::async, [&] { processes.destroyProcess(process); });
    const bool waited = destroyed.wait_for(std::chrono::milliseconds(100)) !=
                        std::future_status::ready;
    gate->setSignaledValue(1);
    check(destroyed.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "Process destruction resumes after the GPU drain");
    destroyed.get();
    check(waited && !executed && spaces[0]->gpuWorkCancelled() &&
              spaces[1]->gpuWorkCancelled() && device.getPfifo().waitForCompletion(serial) &&
              static_cast<const u8*>(memory->contents())[0] == 0x7f,
          "Process cleanup cancels pending jobs and drains committed GPU work before freeing memory");
    std::cout << "PASS: process lifetime, pending cancellation and actual GPU drain\n";
}

void testSemaphoreShutdown(System& system, kernel::Process* process) {
    namespace gpu = hw::tegra_x1::gpu;
    auto& device = system.getGpu();
    auto& gmmu = process->getGMmu();
    alignas(16) std::array<u32, 2> semaphore{};
    constexpr uptr SEMAPHORE = 0x4000000;
    constexpr uptr COMMANDS = 0x4001000;
    const auto immediate = [](u32 method, u32 value) {
        return (4u << 29) | (value << 16) | method;
    };
    const std::array<u32, 11> commands{
        (1u << 29) | (2u << 16) | 4u, 0, static_cast<u32>(SEMAPHORE + 4),
        immediate(0x1b, 1),
        (1u << 29) | (1u << 16) | 5u, static_cast<u32>(SEMAPHORE),
        immediate(6, 1), immediate(7, 1),
        immediate(0xd0e, 0x777), immediate(0xd0f, 0x888), immediate(2, 0)};
    gmmu.map(SEMAPHORE, {reinterpret_cast<uptr>(semaphore.data()), sizeof(semaphore)});
    gmmu.map(COMMANDS, {reinterpret_cast<uptr>(commands.data()), sizeof(commands)});
    const auto entry = std::bit_cast<gpu::GpfifoEntry>(
        static_cast<u64>(COMMANDS) | (static_cast<u64>(commands.size()) << 42));
    std::atomic<bool> completed{false};
    device.getPfifo().submitEntries(gmmu, {&entry, 1}, gpu::GpfifoFlags::None,
                                    [&] { completed = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::atomic_ref<u32>(semaphore[1]).load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(semaphore[1] == 1, "PFIFO reached the pending semaphore acquire");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto stopping = std::chrono::steady_clock::now();
    device.getPfifo().shutdown();
    check(!completed && device.get3DEngine().getReg(0xd0e) == 0 &&
              std::chrono::steady_clock::now() - stopping < std::chrono::seconds(1),
          "Shutdown cancels a pending acquire without executing subsequent methods");
    gmmu.unmap(SEMAPHORE);
    gmmu.unmap(COMMANDS);
    std::cout << "PASS: semaphore acquire cancellation and no execution after shutdown\n";
}

int main(int argc, char** argv) {
    try {
        Ui ui;
        if (argc == 2 && std::string_view(argv[1]) == "--check-channel-faults") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("Channel fault test");
            testChannelFaults(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-process-cancel") {
            unsetenv("HYDRA_GPU_SYNC");
            System system(ui);
            testGpuProcessCancellation(system);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-channel-scheduling") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("Channel scheduling test");
            testChannelScheduling(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-semaphore-shutdown") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("Semaphore shutdown test");
            testSemaphoreShutdown(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-pfifo-decode") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("PFIFO decode test");
            testPfifoDecode(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-as-binding") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("GPU AS binding test");
            testGpuAddressSpaceBinding(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-heap-resize") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("GPU heap resize test");
            testGpuBackingFollowsHeapResize(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-as-lifecycle") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("GPU AS lifecycle test");
            testGpuAddressSpaceLifecycle(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpu-address-space") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("GPU address space test");
            testGpuAddressSpaceInput(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-gpfifo-input") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("GPFIFO input test");
            testGpfifoInput(system, process);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--check-video-events") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager()
                                .createProcess("Video event test");
            testVideoEvents(system, process);
            return 0;
        }
#ifdef HYDRA_FFMPEG_VIDEO
        if (argc == 2 && std::string_view(argv[1]) == "--check-video-writeback") {
            System system(ui);
            auto* process = system.getOs().getKernel().getProcessManager().createProcess("Video writeback test");
            testNvdecWriteback(system, process);
            std::cout << "PASS: NVDEC and VIC NV12/RGB matrix writeback in both layouts\n";
            return 0;
        }
#endif
        for (int iteration = 0; iteration < 5; ++iteration) {
            std::atomic<bool> finished{false}, child_finished{false};
            std::promise<void> started;
            {
                System system(ui);
                auto* process = system.getOs()
                                    .getKernel()
                                    .getProcessManager()
                                    .createProcess("Shutdown test");
                testDomainShutdown(system, process);
                testVideoCapture(system, process);
                testVideoEvents(system, process);
#ifdef HYDRA_FFMPEG_VIDEO
                testNvdecWriteback(system, process);
#endif
                testConditionVariableFlag(system, process);
                testClockRequests(system, process);
                testAddOnContentEvent(system, process);
                testAudioMixUpdates(system, process);
                testAudioOutShutdown(system, process);
                auto* thread = new kernel::HostThread(
                    process, 0x20, [&](const kernel::should_stop_fn_t& stop) {
                        auto* current = kernel::tls_current_thread;
                        auto* tls = reinterpret_cast<u8*>(current->getTlsPtr());
                        tls[0] = 0x5a;
                        started.set_value();
                        while (!stop())
                            std::this_thread::yield();
                        // Starting a child during stop must not escape the join
                        // snapshot.
                        auto* child = new kernel::HostThread(
                            process, 0x20,
                            [&](const kernel::should_stop_fn_t& child_stop) {
                                while (!child_stop())
                                    std::this_thread::yield();
                                child_finished = true;
                            });
                        process->addHandleNoRetain(child);
                        child->start();
                        check(tls[0] == 0x5a,
                              "TLS survives until callback returns");
                        finished = true;
                    });
                process->addHandleNoRetain(thread);
                thread->start();
                check(started.get_future().wait_for(std::chrono::seconds(5)) ==
                          std::future_status::ready,
                      "Worker started");
                if (iteration % 2 == 0) {
                    system.forceStop();
                    system.forceStop();
                }
                // Other iterations exercise destructor-only shutdown, including
                // services/PFIFO.
            }
            check(finished && child_finished,
                  "System joins parent and late-starting child");
        }
        std::cout << "PASS: repeated System teardown, late child startup, live "
                     "TLS, idempotent stop\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

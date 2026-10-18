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
#include <filesystem>
#include <fstream>
#include "core/horizon/ui/handler_base.hpp"
#include "core/system.hpp"
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
        std::array<u32, 13> fenced_submission{1, 0, 1, 1, handle->getRaw(), 0xc000,
            vic_commands.size(), syncpoint, 2, 0xffffffff, 0xffffffff, 0xffffffff, 0};
        std::array<u32, 13> fenced_response{};
        vic_context.in_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(fenced_submission.data()), sizeof(fenced_submission)));
        vic_context.out_stream = ztd::io::MemoryStream(std::span(reinterpret_cast<u8*>(fenced_response.data()), sizeof(fenced_response)));
        check(vic.ioctl(vic_context, 0, 1) == services::nvdrv::NvResult::Success,
              "VIC consumes the NVDEC surface selected by guest commands");
        check(fenced_response[12] == 2 && fenced_response[4] == handle->getRaw(),
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
        check(wait(2) == services::nvdrv::NvResult::Success && wait(3) == services::nvdrv::NvResult::Timeout,
              "Syncpoint waits report actual completion rather than succeeding with zero");
        configWord(0x20, 29); // Unsupported 24-bit output must not signal completion.
        check(vic.ioctl(vic_context, 0, 1) == services::nvdrv::NvResult::NotSupported &&
              system.getGpu().getVideoSyncpoints().read(syncpoint) == 2,
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
    check(call(0x1e, request, response) == nv::NvResult::BadValue,
          "Pending async video wait returned wrong status");
    check(counters.complete(*id, 1).has_value() && !event->clear(),
          "Video event signalled before its threshold");
    check(counters.complete(*id, 1).has_value() && event->clear(),
          "Completing video counter did not signal its waiting event");
    check(call(0x1e, request, response) == nv::NvResult::Success && event->clear(),
          "Already-completed async video wait did not signal its event");
    request[1] = 3;
    check(call(0x1e, request, response) == nv::NvResult::BadValue,
          "Cannot arm another video event wait");
    check(call(0x20, slot, registered) == nv::NvResult::Success &&
              counters.complete(*id, 1).has_value(),
          "Unregistering a pending video event did not safely cancel it");
    std::cout << "PASS: asynchronous video event completion, rearm and cancellation\n";
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
    const auto pointer_call = [&](u64 address, u32 nr) {
        std::array<u8, 24> request{};
        std::array<u8, 24> response{};
        const u32 count = 2;
        std::memcpy(request.data(), &address, 8);
        std::memcpy(request.data() + 8, &count, 4);
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(request),
            .out_stream = ztd::io::MemoryStream(response)};
        return channel.ioctl(context, 0x48, nr);
    };
    for (u32 nr : {0x08u, 0x1bu}) {
        check(pointer_call(guest + 0xffc, nr) == nv::NvResult::Success &&
                  channel.captured.size() == 2 &&
                  std::memcmp(channel.captured.data(), pointed_entries.data(), 16) == 0,
              "Ioctl1 reads the guest pointer array across a CPU page boundary");
        const auto before = channel.calls;
        check(pointer_call(guest + 0x1ffc, nr) == nv::NvResult::InvalidAddress &&
                  channel.calls == before,
              "Unmapped guest-pointer array tail is rejected before submission");
        check(pointer_call(0xfffffffffffffff8ull, nr) == nv::NvResult::InvalidAddress,
              "Guest pointer arithmetic cannot wrap into mapped memory");
    }
    nv::ioctl::NvHostGpu real_channel;
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
    const auto submit_channel = [&](nv::ioctl::NvHostGpu& channel,
                                    std::span<const u32> words) {
        std::array<u32, 8> request{0, 0, 1,
            static_cast<u32>(gpu::GpfifoFlags::FenceGet), 0, 0, 0, 0};
        const auto command = entry(map_words(words), static_cast<u32>(words.size()));
        std::memcpy(request.data() + 6, &command, sizeof(command));
        const auto bytes = std::span(reinterpret_cast<u8*>(request.data()), sizeof(request));
        nv::ioctl::IoctlContext context{
            .system = system, .process = process,
            .in_stream = ztd::io::MemoryStream(bytes),
            .out_stream = ztd::io::MemoryStream(bytes)};
        check(channel.ioctl(context, 0x48, 0x1b) == nv::NvResult::Success &&
                  device.getVideoSyncpoints().waitFor(request[4], request[5],
                                                      std::chrono::milliseconds(1000)),
              "Channel submission reaches its completion fence");
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
    context.in_stream->seekTo(0);
    context.out_stream->seekTo(0);
    check(channel.ioctl(context, 0x48, 0x1b) == nv::NvResult::InvalidState,
          "Faulted channel rejects further submissions");
    nv::ioctl::NvHostGpu unsupported;
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
              state->error == 25,
          "Unsupported conditional entry faults instead of executing unconditionally");
    while (system.getGpu().getVideoSyncpoints().allocate()) {}
    nv::ioctl::NvHostGpu exhausted;
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
    check(waited && !executed && device.getPfifo().waitForCompletion(serial) &&
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

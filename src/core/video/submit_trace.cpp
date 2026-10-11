#include "core/video/submit_trace.hpp"

#include "core/video/submit.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace hydra::video {
namespace {
bool writeFile(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

// CPU mappings need not be contiguous across pages. Never dereference a guest
// range before validating each page's host translation.
std::optional<std::vector<u8>> readGuest(hw::tegra_x1::cpu::IMmu& mmu,
                                        u64 address, usize size) {
    std::vector<u8> result(size);
    usize copied = 0;
    while (copied < size) {
        const auto current = address + copied;
        if (current < address)
            return std::nullopt;
        const auto host = mmu.unmapAddr(current);
        if (!host)
            return std::nullopt;
        const auto amount = std::min(size - copied,
                                     usize(0x1000 - (current & 0xfff)));
        std::memcpy(result.data() + copied, reinterpret_cast<const void*>(host), amount);
        copied += amount;
    }
    return result;
}
} // namespace

void traceEngineState(std::string_view engine,
                      horizon::services::nvdrv::ioctl::IoctlContext& context,
                      std::span<const u32> registers) {
    const char* directory = std::getenv("HYDRA_DUMP_VIDEO");
    if (!directory || !context.process)
        return;
    static std::atomic<u32> count{0};
    const auto sequence = count.fetch_add(1);
    if (sequence >= 16)
        return;
    try {
        const std::filesystem::path root(directory);
        std::filesystem::create_directories(root);
        const auto prefix = fmt::format("engine-{:04}-{}", sequence, engine);
        writeFile(root / (prefix + "-registers.bin"),
                  {reinterpret_cast<const u8*>(registers.data()), registers.size_bytes()});
        auto read = [&](u32 reg, usize size) -> std::optional<std::vector<u8>> {
            const u64 address = u64(registers[reg]) << 8;
            const auto guest = context.system.getGpu().resolveHost1x(address, size);
            if (!guest)
                return std::nullopt;
            return readGuest(*context.process->getMmu(), *guest, size);
        };
        if (engine == "nvdec" && registers[0x80] == 3) {
            const auto picture = read(0x101, 0x2fc);
            if (!picture) {
                LOG_WARN(Services, "NVDEC picture address 0x{:x} is unmapped", u64(registers[0x101]) << 8);
                return;
            }
            writeFile(root / (prefix + "-picture.bin"), *picture);
            u32 size;
            std::memcpy(&size, picture->data() + 0x48, sizeof(size));
            if (size && size <= 32 * 1024 * 1024) {
                const auto stream = read(0x102, size);
                if (stream)
                    writeFile(root / (prefix + "-h264.bin"), *stream);
            }
            LOG_INFO(Services, "Captured NVDEC H264 picture and {} stream bytes", size);
        } else if (engine == "vic") {
            const auto config = read(0x1c2, 0x610);
            if (config)
                writeFile(root / (prefix + "-config.bin"), *config);
        }
    } catch (const std::exception& error) {
        LOG_WARN(Services, "Video engine capture failed: {}", error.what());
    }
}

void traceIoctl(std::string_view engine,
                horizon::services::nvdrv::ioctl::IoctlContext& context, u32 code) {
    const char* directory = std::getenv("HYDRA_DUMP_VIDEO");
    if (!directory || engine.empty())
        return;
    static std::atomic<u32> count{0};
    const auto sequence = count.fetch_add(1);
    if (sequence >= 128)
        return;
    try {
        const std::filesystem::path root(directory);
        std::filesystem::create_directories(root);
        const auto name = fmt::format("ioctl-{:04}-{}-{:08x}.bin", sequence, engine, code);
        std::span<const u8> bytes;
        if (context.in_stream) {
            auto& stream = *context.in_stream;
            if (stream.getSeek() > stream.getSize() ||
                stream.getSize() - stream.getSeek() > 65536)
                return;
            bytes = std::span<const u8>(stream.getPtr() + stream.getSeek(),
                                        static_cast<usize>(stream.getSize() - stream.getSeek()));
        }
        if (writeFile(root / name, bytes))
            LOG_INFO(Services, "Captured {} ioctl 0x{:08x} ({} bytes) to {}/{}",
                      engine, code, bytes.size(), directory, name);
        else
            LOG_WARN(Services, "Cannot write video ioctl capture {}", name);
    } catch (const std::exception& error) {
        LOG_WARN(Services, "Video ioctl capture failed: {}", error.what());
    }
}

void traceSubmission(std::string_view engine,
                     horizon::services::nvdrv::ioctl::IoctlContext& context) {
    const char* directory = std::getenv("HYDRA_DUMP_VIDEO");
    if (!directory || engine.empty() || !context.in_stream || !context.process)
        return;
    // Limit capture to 16 jobs and 4 MiB total command bytes per job.
    static std::atomic<u32> capture_count{0};
    const auto sequence = capture_count.fetch_add(1);
    if (sequence >= 16)
        return;
    try {
        auto& stream = *context.in_stream;
        if (stream.getSeek() > stream.getSize())
            return;
        const auto remaining = stream.getSize() - stream.getSeek();
        if (remaining > 4 * 1024 * 1024)
            return;
        const std::span<const u8> bytes(stream.getPtr() + stream.getSeek(),
                                       static_cast<usize>(remaining));
        const std::filesystem::path root(directory);
        std::filesystem::create_directories(root);
        const auto prefix = fmt::format("{:04}-{}", sequence, engine);
        if (!writeFile(root / (prefix + "-submit.bin"), bytes)) {
            LOG_WARN(Services, "Cannot write video submit capture {}", prefix);
            return;
        }
        const auto parsed = parseSubmission(bytes);
        if (!parsed) {
            LOG_WARN(Services, "Video capture {}: {}", prefix, parsed.error());
            return;
        }
        std::ofstream metadata(root / (prefix + "-commands.txt"));
        metadata << fmt::format("commands={} relocations={} syncpoints={} fences={}\n",
                                parsed->commands.size(), parsed->relocation_count,
                                parsed->syncpoint_count, parsed->fence_count);
        usize budget = 4 * 1024 * 1024;
        usize captured = 0;
        for (usize i = 0; i < parsed->commands.size(); ++i) {
            const auto& command = parsed->commands[i];
            const auto map = context.system.getGpu().getMap(Handle(command.handle));
            const auto size = usize(command.word_count) * 4;
            if (!map || command.offset > (*map)->size ||
                size > (*map)->size - command.offset || size > budget) {
                metadata << fmt::format("{}: invalid or oversized handle={} offset={} words={}\n",
                                        i, command.handle, command.offset, command.word_count);
                continue;
            }
            budget -= size;
            const auto address = (*map)->addr + command.offset;
            if (address < (*map)->addr)
                continue;
            const auto data = readGuest(*context.process->getMmu(), address, size);
            if (!data) {
                metadata << fmt::format("{}: unmapped guest command buffer\n", i);
                continue;
            }
            const auto filename = fmt::format("{}-cmd-{:03}.bin", prefix, i);
            if (writeFile(root / filename, *data)) {
                ++captured;
                metadata << fmt::format("{}: handle={} offset={} words={} file={}\n",
                                        i, command.handle, command.offset,
                                        command.word_count, filename);
            }
        }
        LOG_INFO(Services, "Captured {}/{} video command buffers to {}/{}",
                  captured, parsed->commands.size(), directory, prefix);
    } catch (const std::exception& error) {
        LOG_WARN(Services, "Video command capture failed: {}", error.what());
    }
}
} // namespace hydra::video

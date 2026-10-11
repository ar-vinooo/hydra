#include "core/horizon/filesystem/ncz_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <unistd.h>
#include <zstd.h>

namespace hydra::horizon::filesystem {
namespace {

template <typename T>
T valueAt(std::span<const u8> data, usize offset) {
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("Truncated NCZ header.");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::vector<u8> readAt(ztd::io::IStream& stream, u64 offset, u64 size) {
    stream.seekTo(offset);
    std::vector<u8> data(size);
    stream.readToSpan(std::span(data));
    return data;
}

void writeAll(int fd, const u8* data, usize size, u64 offset) {
    while (size != 0) {
        const auto written = ::pwrite(fd, data, size, static_cast<off_t>(offset));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(fmt::format(
                "Failed to write NCZ cache: {}", std::strerror(errno)));
        }
        data += written;
        size -= static_cast<usize>(written);
        offset += static_cast<u64>(written);
    }
}

void readAll(int fd, std::span<u8> output, u64 offset) {
    while (!output.empty()) {
        const auto count = ::pread(fd, output.data(), output.size(),
                                   static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("Failed to read NCZ cache.");
        output = output.subspan(static_cast<usize>(count));
        offset += static_cast<u64>(count);
    }
}

// Removes partial caches left behind by processes that no longer exist.
void removeStaleParts(const std::filesystem::path& cache_path) {
    std::error_code ec;
    const auto prefix = cache_path.filename().string() + ".";
    for (const auto& entry : std::filesystem::directory_iterator(
             cache_path.parent_path(), ec)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(".part"))
            continue;
        const auto pid = std::atoi(name.c_str() + prefix.size());
        if (pid > 0 && (::kill(pid, 0) == 0 || errno != ESRCH))
            continue;
        std::filesystem::remove(entry.path(), ec);
    }
}

} // namespace

struct NczFile::State {
    std::shared_ptr<IFile> container;
    u64 offset;
    u64 size;
    u64 nca_size;
    // Absolute container offset of the compressed body.
    u64 data_offset;

    // Block compression
    bool block{false};
    u64 block_size{0};
    std::vector<u64> block_offsets;
    std::vector<u32> block_sizes;

    // Solid compression, decompressed progressively into a cache file
    std::mutex mutex;
    std::filesystem::path cache_path;
    std::filesystem::path part_path;
    int fd{-1};
    u64 decoded_end{0};
    ZSTD_DStream* dstream{nullptr};
    std::unique_ptr<ztd::io::IStream> input;
    u64 input_position{0};

    ~State() {
        if (dstream != nullptr)
            ZSTD_freeDStream(dstream);
        if (fd >= 0)
            ::close(fd);
        if (!part_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(part_path, ec);
        }
    }

    u64 bodySize() const { return nca_size - HEADER_SIZE; }

    void openCache() {
        std::error_code ec;
        if (std::filesystem::file_size(cache_path, ec) == bodySize() && !ec) {
            fd = ::open(cache_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                decoded_end = bodySize();
                return;
            }
        }

        std::filesystem::create_directories(cache_path.parent_path(), ec);
        removeStaleParts(cache_path);
        static std::atomic<u32> counter{0};
        part_path = cache_path;
        part_path += fmt::format(".{}.{}.part", ::getpid(), counter++);
        fd = ::open(part_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0644);
        if (fd < 0) {
            part_path.clear();
            throw std::runtime_error(
                fmt::format("Failed to create NCZ cache {}: {}",
                            cache_path.string(), std::strerror(errno)));
        }
        dstream = ZSTD_createDStream();
        if (dstream == nullptr)
            throw std::runtime_error("Failed to create zstd decompressor.");
        ZSTD_initDStream(dstream);
        input.reset(container->open(FileOpenFlags::Read));
        input_position = data_offset;
    }

    // Decompresses the solid stream until at least `target` body bytes exist.
    void decodeTo(u64 target) {
        std::lock_guard lock(mutex);
        if (target <= decoded_end)
            return;

        std::vector<u8> in_buffer(ZSTD_DStreamInSize());
        std::vector<u8> out_buffer(std::max<usize>(ZSTD_DStreamOutSize(),
                                                   4 * 1024 * 1024));
        const u64 input_end = offset + size;
        ZSTD_inBuffer in{in_buffer.data(), 0, 0};
        while (decoded_end < target) {
            if (in.pos == in.size) {
                const auto count = std::min<u64>(in_buffer.size(),
                                                 input_end - input_position);
                input->seekTo(input_position);
                input->readToSpan(std::span(in_buffer).first(count));
                input_position += count;
                in = {in_buffer.data(), static_cast<size_t>(count), 0};
            }
            ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
            const auto result = ZSTD_decompressStream(dstream, &out, &in);
            if (ZSTD_isError(result))
                throw std::runtime_error(
                    fmt::format("Failed to decompress NCZ: {}",
                                ZSTD_getErrorName(result)));
            if (out.pos == 0 && in.pos == in.size &&
                input_position == input_end)
                throw std::runtime_error(
                    "NCZ data ends before the NCA is complete.");
            if (out.pos > bodySize() - decoded_end)
                throw std::runtime_error(
                    "NCZ decompresses to more data than its NCA size.");
            writeAll(fd, out_buffer.data(), out.pos, decoded_end);
            decoded_end += out.pos;
        }
        // Rewind the input to what zstd has not consumed yet.
        input_position -= in.size - in.pos;

        if (decoded_end == bodySize()) {
            ZSTD_freeDStream(dstream);
            dstream = nullptr;
            input.reset();
            std::error_code ec;
            std::filesystem::rename(part_path, cache_path, ec);
            if (ec)
                LOG_WARN(Filesystem, "Failed to keep NCZ cache {}: {}",
                         cache_path.string(), ec.message());
            else
                part_path.clear();
            LOG_INFO(Filesystem, "Decompressed NCZ to {}", cache_path.string());
        }
    }
};

namespace {

class NczStream final : public ztd::io::IStream {
  public:
    explicit NczStream(std::shared_ptr<NczFile::State> state_)
        : state{std::move(state_)},
          source{state->container->open(FileOpenFlags::Read)} {}

    u64 getSeek() const noexcept override { return position; }
    u64 getSize() const noexcept override { return state->nca_size; }
    void seekTo(u64 seek) noexcept override { position = seek; }
    void seekBy(u64 amount) noexcept override { position += amount; }

  protected:
    void readRaw(std::span<u8> output) noexcept override {
        ASSERT(position <= state->nca_size &&
                   output.size() <= state->nca_size - position,
               Filesystem, "NCZ read exceeds file bounds");
        try {
            while (!output.empty()) {
                if (position < NczFile::HEADER_SIZE) {
                    const auto count = std::min<u64>(
                        output.size(), NczFile::HEADER_SIZE - position);
                    source->seekTo(state->offset + position);
                    source->readToSpan(output.first(count));
                    advance(output, count);
                } else if (state->block) {
                    readBlock(output);
                } else {
                    const auto body = position - NczFile::HEADER_SIZE;
                    state->decodeTo(body + output.size());
                    readAll(state->fd, output, body);
                    advance(output, output.size());
                }
            }
        } catch (const std::exception& error) {
            LOG_FATAL(Filesystem, "Failed to read NCZ: {}", error.what());
        }
    }

  private:
    std::shared_ptr<NczFile::State> state;
    std::unique_ptr<ztd::io::IStream> source;
    u64 position{0};
    usize cached_block{invalid<usize>()};
    std::vector<u8> block_data;
    std::vector<u8> compressed;

    void advance(std::span<u8>& output, u64 count) {
        position += count;
        output = output.subspan(count);
    }

    void readBlock(std::span<u8>& output) {
        const auto body = position - NczFile::HEADER_SIZE;
        const auto index = static_cast<usize>(body / state->block_size);
        if (index != cached_block) {
            const auto expected = static_cast<usize>(std::min<u64>(
                state->block_size,
                state->bodySize() - index * state->block_size));
            const auto stored = state->block_sizes[index];
            block_data.resize(expected);
            source->seekTo(state->block_offsets[index]);
            if (stored < expected) {
                compressed.resize(stored);
                source->readToSpan(std::span(compressed));
                const auto result =
                    ZSTD_decompress(block_data.data(), block_data.size(),
                                    compressed.data(), compressed.size());
                if (ZSTD_isError(result) || result != expected)
                    throw std::runtime_error(fmt::format(
                        "Failed to decompress NCZ block {}: {}", index,
                        ZSTD_isError(result) ? ZSTD_getErrorName(result)
                                             : "wrong size"));
            } else {
                // Incompressible blocks are stored as-is.
                source->readToSpan(std::span(block_data));
            }
            cached_block = index;
        }
        const auto skip = body - index * state->block_size;
        const auto count = std::min<u64>(output.size(), block_data.size() - skip);
        std::memcpy(output.data(), block_data.data() + skip, count);
        advance(output, count);
    }
};

} // namespace

NczFile::NczFile(std::shared_ptr<IFile> container, u64 offset, u64 size,
                 u64 nca_size, std::filesystem::path cache_path)
    : state{std::make_shared<State>()} {
    state->container = std::move(container);
    state->offset = offset;
    state->size = size;
    state->nca_size = nca_size;
    state->cache_path = std::move(cache_path);
    if (offset > state->container->getSize() ||
        size > state->container->getSize() - offset ||
        size < HEADER_SIZE + 16 || nca_size <= HEADER_SIZE)
        throw std::runtime_error("Truncated NCZ.");

    std::unique_ptr<ztd::io::IStream> stream(
        state->container->open(FileOpenFlags::Read));
    u64 cursor = offset + HEADER_SIZE;
    const u64 end = offset + size;
    const auto take = [&](u64 count) {
        if (count > end - cursor)
            throw std::runtime_error("Truncated NCZ header.");
        auto data = readAt(*stream, cursor, count);
        cursor += count;
        return data;
    };

    const auto sections_header = take(16);
    if (std::memcmp(sections_header.data(), "NCZSECTN", 8) != 0)
        throw std::runtime_error("Invalid NCZ section magic.");
    const auto count = valueAt<u64>(sections_header, 8);
    if (count == 0 || count > 16)
        throw std::runtime_error("Invalid NCZ section count.");
    const auto sections = take(count * 0x40);
    for (u64 i = 0; i < count; ++i) {
        const auto entry = static_cast<usize>(i * 0x40);
        const auto start = valueAt<u64>(sections, entry);
        const auto length = valueAt<u64>(sections, entry + 8);
        const auto crypto = valueAt<u64>(sections, entry + 16);
        if (start < HEADER_SIZE || start > nca_size ||
            length > nca_size - start)
            throw std::runtime_error("Invalid NCZ section range.");
        // Only unencrypted and AES-CTR sections are stored decrypted.
        if (crypto != 1 && crypto != 3 && crypto != 4)
            throw std::runtime_error(fmt::format(
                "Unsupported NCZ section encryption type {}.", crypto));
    }

    if (end - cursor >= 8) {
        const auto magic = readAt(*stream, cursor, 8);
        state->block = std::memcmp(magic.data(), "NCZBLOCK", 8) == 0;
    }
    if (state->block) {
        const auto header = take(24);
        const auto exponent = header[11];
        const auto blocks = valueAt<u32>(header, 12);
        const auto decompressed = valueAt<u64>(header, 16);
        if (exponent < 14 || exponent > 32)
            throw std::runtime_error("Invalid NCZ block size.");
        state->block_size = u64{1} << exponent;
        if (decompressed != state->bodySize() ||
            blocks != (decompressed + state->block_size - 1) /
                          state->block_size)
            throw std::runtime_error(
                "NCZ block table does not match the NCA size.");
        const auto table = take(static_cast<u64>(blocks) * 4);
        state->block_sizes.resize(blocks);
        state->block_offsets.resize(blocks);
        u64 block_offset = cursor;
        for (u32 i = 0; i < blocks; ++i) {
            const auto stored = valueAt<u32>(table, i * 4);
            if (stored == 0 || stored > end - block_offset)
                throw std::runtime_error("Invalid NCZ block table.");
            state->block_sizes[i] = stored;
            state->block_offsets[i] = block_offset;
            block_offset += stored;
        }
    }
    state->data_offset = cursor;
    if (!state->block)
        state->openCache();
}

ztd::io::IStream* NczFile::open(FileOpenFlags flags) {
    ASSERT(!any(flags & FileOpenFlags::Write), Filesystem,
           "NCZ content is read-only");
    return new NczStream(state);
}

u64 NczFile::getSize() const { return state->nca_size; }

bool NczFile::isBlockCompressed() const { return state->block; }

} // namespace hydra::horizon::filesystem

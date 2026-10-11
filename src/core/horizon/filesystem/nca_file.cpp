#include "core/horizon/filesystem/nca_file.hpp"

#include <CommonCrypto/CommonCryptor.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace hydra::horizon::filesystem {
namespace {

template <typename T>
T valueAt(const NcaFile::Header& header, usize offset) {
    T value;
    std::memcpy(&value, header.data() + offset, sizeof(value));
    return value;
}

std::span<const u8> requireKey(const NcaFile::Keys& keys,
                               const std::string& name, usize size) {
    const auto it = keys.find(name);
    if (it == keys.end() || it->second.size() != size)
        throw std::runtime_error(
            fmt::format("prod.keys is missing {}. Import an up-to-date "
                        "prod.keys in Settings > General.",
                        name));
    return it->second;
}

void ecb(CCOperation operation, std::span<const u8> key,
         std::span<const u8> input, std::span<u8> output) {
    size_t written = 0;
    if (CCCrypt(operation, kCCAlgorithmAES, kCCOptionECBMode, key.data(),
                key.size(), nullptr, input.data(), input.size(), output.data(),
                output.size(), &written) != kCCSuccess ||
        written != input.size())
        throw std::runtime_error("AES decryption failed.");
}

void xtsDecrypt(std::span<const u8> key, std::span<u8> sector, u64 number) {
    std::array<u8, 16> tweak{};
    // Nintendo encodes the sector number in big endian, unlike standard XTS.
    for (usize i = 0; i < 8; ++i) {
        tweak[15 - i] = static_cast<u8>(number & 0xff);
        number >>= 8;
    }
    ecb(kCCEncrypt, key.subspan(16, 16), tweak, tweak);
    std::array<u8, 0x200> tweaks{};
    for (usize block = 0; block < sector.size(); block += 16) {
        std::copy(tweak.begin(), tweak.end(),
                  tweaks.begin() + static_cast<std::ptrdiff_t>(block));
        for (usize j = 0; j < 16; ++j)
            sector[block + j] ^= tweak[j];
        u8 carry = 0;
        for (auto& byte : tweak) {
            const auto next = static_cast<u8>(byte >> 7);
            byte = static_cast<u8>((byte << 1) | carry);
            carry = next;
        }
        if (carry != 0)
            tweak[0] ^= 0x87;
    }
    ecb(kCCDecrypt, key.first(16), sector, sector);
    for (usize i = 0; i < sector.size(); ++i)
        sector[i] ^= tweaks[i];
}

} // namespace

struct NcaFile::State {
    struct Section {
        u64 offset;
        u64 size;
        u8 encryption;
        std::array<u8, 16> counter;
    };
    std::shared_ptr<IFile> container;
    u64 offset;
    u64 size;
    Header header;
    std::array<u8, 0x40> key_area{};
    std::vector<Section> sections;
};

namespace {

class NcaStream final : public ztd::io::IStream {
  public:
    explicit NcaStream(std::shared_ptr<NcaFile::State> state_)
        : state{std::move(state_)},
          source{state->container->open(FileOpenFlags::Read)} {}

    u64 getSeek() const noexcept override { return position; }
    u64 getSize() const noexcept override { return state->size; }
    void seekTo(u64 seek) noexcept override { position = seek; }
    void seekBy(u64 amount) noexcept override { position += amount; }

  protected:
    void readRaw(std::span<u8> output) noexcept override {
        ASSERT(position <= state->size &&
                   output.size() <= state->size - position,
               Filesystem, "NCA read exceeds file bounds");
        try {
            while (!output.empty()) {
                if (position < state->header.size()) {
                    const auto count = std::min<u64>(
                        output.size(), state->header.size() - position);
                    std::memcpy(output.data(), state->header.data() + position,
                                count);
                    position += count;
                    output = output.subspan(count);
                    continue;
                }
                const NcaFile::State::Section* section = nullptr;
                u64 end = state->size;
                for (const auto& candidate : state->sections) {
                    if (position >= candidate.offset &&
                        position < candidate.offset + candidate.size) {
                        section = &candidate;
                        end = candidate.offset + candidate.size;
                        break;
                    }
                    if (candidate.offset > position)
                        end = std::min(end, candidate.offset);
                }
                const auto count = std::min<u64>(output.size(), end - position);
                auto chunk = output.first(count);
                if (section == nullptr || section->encryption == 1) {
                    source->seekTo(state->offset + position);
                    source->readToSpan(chunk);
                } else if (section->encryption == 3) {
                    readCtr(*section, chunk);
                } else {
                    readXts(*section, chunk);
                }
                position += count;
                output = output.subspan(count);
            }
        } catch (const std::exception& error) {
            LOG_FATAL(Filesystem, "Failed to read NCA: {}", error.what());
        }
    }

  private:
    std::shared_ptr<NcaFile::State> state;
    std::unique_ptr<ztd::io::IStream> source;
    u64 position{0};
    std::array<u8, 0x10000> ctr_cache{};
    u64 ctr_cache_offset{0};
    usize ctr_cache_size{0};

    void readCtr(const NcaFile::State::Section& section, std::span<u8> output) {
        auto cursor = position;
        while (!output.empty()) {
            if (output.size() >= ctr_cache.size()) {
                decryptCtr(section, cursor, output);
                return;
            }
            if (ctr_cache_size == 0 || cursor < ctr_cache_offset ||
                cursor >= ctr_cache_offset + ctr_cache_size) {
                ctr_cache_offset =
                    std::max(section.offset, cursor & ~u64{0xffff});
                ctr_cache_size = static_cast<usize>(std::min<u64>(
                    ctr_cache.size(),
                    section.offset + section.size - ctr_cache_offset));
                decryptCtr(section, ctr_cache_offset,
                           std::span(ctr_cache).first(ctr_cache_size));
            }
            const auto skip = cursor - ctr_cache_offset;
            const auto count =
                std::min<u64>(output.size(), ctr_cache_size - skip);
            std::memcpy(output.data(), ctr_cache.data() + skip, count);
            cursor += count;
            output = output.subspan(count);
        }
    }

    void decryptCtr(const NcaFile::State::Section& section, u64 cursor,
                    std::span<u8> output) {
        auto counter = section.counter;
        auto block = cursor / 16;
        for (usize i = 0; i < 8; ++i) {
            counter[15 - i] = static_cast<u8>(block & 0xff);
            block >>= 8;
        }
        CCCryptorRef cryptor = nullptr;
        if (CCCryptorCreateWithMode(
                kCCDecrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding,
                counter.data(), state->key_area.data() + 0x20, 16, nullptr, 0,
                0, kCCModeOptionCTR_BE, &cryptor) != kCCSuccess)
            throw std::runtime_error("Failed to initialize NCA AES-CTR.");
        // Advance the keystream for an unaligned read, then decrypt in place.
        std::array<u8, 16> prefix{};
        size_t written = 0;
        const auto skip = cursor % 16;
        auto result = CCCryptorUpdate(cryptor, prefix.data(), skip,
                                      prefix.data(), prefix.size(), &written);
        source->seekTo(state->offset + cursor);
        source->readToSpan(output);
        if (result == kCCSuccess)
            result = CCCryptorUpdate(cryptor, output.data(), output.size(),
                                     output.data(), output.size(), &written);
        CCCryptorRelease(cryptor);
        if (result != kCCSuccess || written != output.size())
            throw std::runtime_error("Failed to decrypt NCA AES-CTR data.");
    }

    void readXts(const NcaFile::State::Section& section, std::span<u8> output) {
        auto cursor = position;
        while (!output.empty()) {
            const auto start = cursor & ~u64{0x1ff};
            std::array<u8, 0x200> sector{};
            source->seekTo(state->offset + start);
            source->readToSpan(std::span<u8>(sector));
            xtsDecrypt(std::span(state->key_area).first(32), sector,
                       (start - section.offset) / 0x200);
            const auto skip = cursor - start;
            const auto count =
                std::min<u64>(output.size(), sector.size() - skip);
            std::memcpy(output.data(), sector.data() + skip, count);
            cursor += count;
            output = output.subspan(count);
        }
    }
};

} // namespace

NcaFile::Keys NcaFile::loadKeys(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("XCI requires prod.keys. Import it in "
                                 "Settings > General > Import Keys.");
    Keys keys;
    std::string line;
    while (std::getline(input, line)) {
        line = line.substr(0, line.find_first_of("#;"));
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        auto name = line.substr(0, separator);
        auto hex = line.substr(separator + 1);
        auto removeWhitespace = [](std::string& text) {
            std::erase_if(text,
                          [](unsigned char c) { return std::isspace(c) != 0; });
        };
        removeWhitespace(name);
        removeWhitespace(hex);
        if (hex.size() != 32 && hex.size() != 64)
            continue; // Ignore unrelated key formats.
        std::vector<u8> value;
        for (usize i = 0; i < hex.size(); i += 2) {
            unsigned int byte = 0;
            const auto result =
                std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16);
            if (result.ec != std::errc{} || result.ptr != hex.data() + i + 2)
                throw std::runtime_error(fmt::format(
                    "Invalid hex value for {} in prod.keys.", name));
            value.push_back(static_cast<u8>(byte));
        }
        keys.insert_or_assign(name, std::move(value));
    }
    return keys;
}

NcaFile::Keys NcaFile::loadDefaultKeys() {
    const auto path = std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) /
                      "keys/prod.keys";
    if (std::filesystem::exists(path))
        return loadKeys(path);
    if (const auto* home = std::getenv("HOME")) {
        const auto fallback = std::filesystem::path(home) / ".switch/prod.keys";
        if (std::filesystem::exists(fallback))
            return loadKeys(fallback);
    }
    return {};
}

NcaFile::Header NcaFile::readHeader(IFile& file, u64 offset, u64 size,
                                    const Keys& keys, bool& plaintext) {
    if (size < 0xc00 || offset > file.getSize() ||
        size > file.getSize() - offset)
        throw std::runtime_error("Truncated NCA inside XCI.");
    std::unique_ptr<ztd::io::IStream> stream(file.open(FileOpenFlags::Read));
    stream->seekTo(offset);
    Header header;
    stream->readToSpan(std::span<u8>(header));
    plaintext = valueAt<u32>(header, 0x200) == makeMagic4('N', 'C', 'A', '3');
    if (!plaintext) {
        const auto key = requireKey(keys, "header_key", 32);
        for (usize i = 0; i < header.size(); i += 0x200)
            xtsDecrypt(key, std::span(header).subspan(i, 0x200), i / 0x200);
    }
    if (valueAt<u32>(header, 0x200) != makeMagic4('N', 'C', 'A', '3'))
        throw std::runtime_error("Cannot decrypt XCI NCA header. Check "
                                 "prod.keys; only NCA3 is supported.");
    return header;
}

NcaFile::NcaFile(std::shared_ptr<IFile> container, u64 offset, u64 size,
                 Header header, bool plaintext, const Keys& keys,
                 bool decrypted_body)
    : state{std::make_shared<State>()} {
    state->container = std::move(container);
    state->offset = offset;
    state->size = size;
    state->header = header;
    if (valueAt<u64>(header, 0x208) != size)
        throw std::runtime_error(
            "XCI NCA content size does not match its partition entry.");
    if (!plaintext && !decrypted_body) {
        const auto generation = std::max(header[0x206], header[0x220]);
        const auto revision = generation == 0 ? 0 : generation - 1;
        if (std::ranges::any_of(std::span(header).subspan(0x230, 16),
                                [](u8 byte) { return byte != 0; })) {
            std::string name = "encrypted_titlekey_";
            for (const auto byte : std::span(header).subspan(0x230, 16))
                name += fmt::format("{:02x}", byte);
            const auto ticket = keys.find(name);
            if (ticket == keys.end() || ticket->second.size() != 16)
                throw std::runtime_error(
                    "A matching common ticket is required for this NSP. "
                    "Personalized tickets are not supported.");
            const auto kek =
                requireKey(keys, fmt::format("titlekek_{:02x}", revision), 16);
            ecb(kCCDecrypt, kek, ticket->second,
                std::span(state->key_area).subspan(0x20, 16));
        } else {
            constexpr std::array names{"application", "ocean", "system"};
            if (header[0x207] >= names.size())
                throw std::runtime_error("Invalid NCA key-area index.");
            const auto name = fmt::format("key_area_key_{}_{:02x}",
                                          names[header[0x207]], revision);
            const auto key = requireKey(keys, name, 16);
            ecb(kCCDecrypt, key, std::span(header).subspan(0x300, 0x40),
                state->key_area);
        }
    }
    for (usize i = 0; i < 4; ++i) {
        const auto start =
            static_cast<u64>(valueAt<u32>(header, 0x240 + i * 16)) * 0x200;
        if (start == 0)
            continue;
        const auto end =
            static_cast<u64>(valueAt<u32>(header, 0x244 + i * 16)) * 0x200;
        if (start < header.size() || start >= end || end > size)
            throw std::runtime_error("Invalid NCA section bounds.");
        for (const auto& section : state->sections) {
            if (start < section.offset + section.size && end > section.offset)
                throw std::runtime_error("Overlapping NCA sections.");
        }
        const auto fs = 0x400 + i * 0x200;
        auto encryption = plaintext ? u8{1} : header[fs + 4];
        if (encryption < 1 || encryption > 3)
            throw std::runtime_error("XCI update/patch sections are not "
                                     "supported. Load the base game instead.");
        if (valueAt<u32>(header, fs + 0x148) != 0 ||
            valueAt<u64>(header, fs + 0x160) != 0 ||
            valueAt<u64>(header, fs + 0x178) != 0)
            throw std::runtime_error(
                "Sparse or compressed NCA sections are not supported.");
        if (decrypted_body)
            encryption = 1;
        State::Section section{start, end - start, encryption, {}};
        std::reverse_copy(
            header.begin() + static_cast<std::ptrdiff_t>(fs + 0x140),
            header.begin() + static_cast<std::ptrdiff_t>(fs + 0x148),
            section.counter.begin());
        state->sections.push_back(section);
    }
}

ztd::io::IStream* NcaFile::open(FileOpenFlags flags) {
    ASSERT(!any(flags & FileOpenFlags::Write), Filesystem,
           "XCI content is read-only");
    return new NcaStream(state);
}

u64 NcaFile::getSize() const { return state->size; }

} // namespace hydra::horizon::filesystem

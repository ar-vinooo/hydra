#include "core/horizon/online/online.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "common/config.hpp"
#include "common/toml_helper.hpp"

namespace hydra::horizon::online {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

// SHA-256 and HMAC-SHA256, for the RyuNET proof

constexpr std::array<u32, 64> SHA256_K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

using Digest = std::array<u8, 32>;

class Sha256 {
  public:
    void update(std::span<const u8> data) {
        total_size += data.size();
        for (const auto byte : data) {
            block[block_size++] = byte;
            if (block_size == block.size()) {
                transform();
                block_size = 0;
            }
        }
    }

    Digest finish() {
        const u64 bit_size = total_size * 8;
        block[block_size++] = 0x80;
        if (block_size > 56) {
            std::fill(block.begin() + block_size, block.end(), 0);
            transform();
            block_size = 0;
        }
        std::fill(block.begin() + block_size, block.begin() + 56, 0);
        for (u32 i = 0; i < 8; i++)
            block[56 + i] = static_cast<u8>(bit_size >> (56 - i * 8));
        transform();

        Digest digest;
        for (u32 i = 0; i < 32; i++)
            digest[i] = static_cast<u8>(state[i / 4] >> (24 - (i % 4) * 8));
        return digest;
    }

  private:
    std::array<u32, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                0xa54ff53a, 0x510e527f, 0x9b05688c,
                                0x1f83d9ab, 0x5be0cd19};
    std::array<u8, 64> block{};
    usize block_size{0};
    u64 total_size{0};

    static u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

    void transform() {
        std::array<u32, 64> w;
        for (u32 i = 0; i < 16; i++)
            w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) |
                   (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
        for (u32 i = 16; i < 64; i++) {
            const u32 s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = state;
        for (u32 i = 0; i < 64; i++) {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = h + s1 + ch + SHA256_K[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + s0 + maj;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
};

std::span<const u8> asBytes(std::string_view str) {
    return {reinterpret_cast<const u8*>(str.data()), str.size()};
}

Digest sha256(std::span<const u8> data) {
    Sha256 sha;
    sha.update(data);
    return sha.finish();
}

Digest hmacSha256(std::span<const u8> key, std::span<const u8> message) {
    std::array<u8, 64> key_block{};
    if (key.size() > key_block.size()) {
        const auto digest = sha256(key);
        std::copy(digest.begin(), digest.end(), key_block.begin());
    } else {
        std::copy(key.begin(), key.end(), key_block.begin());
    }

    std::array<u8, 64> pad;
    for (u32 i = 0; i < 64; i++)
        pad[i] = key_block[i] ^ 0x36;
    Sha256 inner;
    inner.update(pad);
    inner.update(message);
    const auto inner_digest = inner.finish();

    for (u32 i = 0; i < 64; i++)
        pad[i] = key_block[i] ^ 0x5c;
    Sha256 outer;
    outer.update(pad);
    outer.update(inner_digest);
    return outer.finish();
}

std::string toHex(std::span<const u8> data) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(data.size() * 2);
    for (const auto byte : data) {
        hex += DIGITS[byte >> 4];
        hex += DIGITS[byte & 0xf];
    }
    return hex;
}

std::optional<std::vector<u8>> fromHex(std::string_view hex) {
    const auto digit = [](char c) -> i32 {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    if (hex.size() % 2 != 0)
        return std::nullopt;
    std::vector<u8> data(hex.size() / 2);
    for (usize i = 0; i < data.size(); i++) {
        const auto high = digit(hex[i * 2]);
        const auto low = digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        data[i] = static_cast<u8>(high << 4 | low);
    }
    return data;
}

std::string toLowerAscii(std::string_view str) {
    std::string lower(str);
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return lower;
}

std::string randomHex(usize size) {
    static std::mutex mutex;
    static std::mt19937_64 rng{std::random_device{}()};
    std::vector<u8> data(size);
    std::lock_guard lock(mutex);
    for (auto& byte : data)
        byte = static_cast<u8>(rng());
    return toHex(data);
}

// Settings

std::mutex g_mutex;
std::map<u64, std::unique_ptr<Config>> g_titles; // nullptr: offline
// Settings read again while a previous session's sockets may still point to
// them
std::vector<std::unique_ptr<Config>> g_retired;
std::map<std::string, u32> g_server_addresses;
std::map<std::string, std::string> g_key_hashes;

std::filesystem::path getOnlineDirectory() {
    return std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) / "online";
}

bool isValidNickname(std::string_view nickname) {
    if (nickname.size() < 3 || nickname.size() > 16)
        return false;
    return std::all_of(nickname.begin(), nickname.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::unique_ptr<Config> readConfig(u64 title_id) {
    const auto path =
        getOnlineDirectory() / fmt::format("{:016x}.toml", title_id);
    if (!std::filesystem::exists(path))
        return nullptr;

    auto config = std::make_unique<Config>();
    try {
        const auto data = toml::parse(path.string());
        config->enabled = toml::find_or<bool>(data, "enabled", false);
        config->server =
            toml::find_or<std::string>(data, "server", std::string{});
        config->http_port =
            static_cast<u16>(toml::find_or<int>(data, "http_port", 8080));
        config->nickname =
            toml::find_or<std::string>(data, "nickname", std::string{});
        config->redirect_hosts = toml::find_or<std::vector<std::string>>(
            data, "redirect_hosts",
            std::vector<std::string>{"*.demonware.net"});
        config->demonware_key =
            toml::find_or<std::string>(data, "demonware_key", std::string{});
    } catch (const std::exception& e) {
        LOG_ERROR(Services, "Online: failed to read {}: {}", path.string(),
                  e.what());
        return nullptr;
    }

    if (!config->enabled)
        return nullptr;
    if (config->server.empty() || config->nickname.empty()) {
        LOG_ERROR(Services,
                  "Online: {} needs a server and a nickname, the title stays "
                  "offline",
                  path.string());
        return nullptr;
    }
    if (config->http_port == 0)
        config->http_port = 8080;
    if (!isValidNickname(config->nickname))
        LOG_WARN(Services,
                 "Online: nickname \"{}\" is not 3 to 16 letters, digits, _ "
                 "or -, the server may refuse it",
                 config->nickname);

    LOG_INFO(Services,
             "Online: title {:016x} plays online through {} (HTTP port {}) as "
             "{}",
             title_id, config->server, config->http_port, config->nickname);
    return config;
}

// This installation's secret (OCTR's ryunet_online.key): the server lets the
// first key to use a nickname keep it. Only hashes of it are sent.
std::optional<std::vector<u8>> loadInstallationKey() {
    const auto dir = getOnlineDirectory();
    const auto path = dir / "ryunet.key";

    std::string hex;
    if (std::ifstream ifs(path); ifs)
        ifs >> hex;
    if (auto key = fromHex(hex); key && key->size() == 32)
        return key;

    hex = randomHex(32);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream ofs(path);
    ofs << hex << '\n';
    if (!ofs) {
        LOG_ERROR(Services,
                  "Online: could not save {}, the nickname cannot be kept",
                  path.string());
        return std::nullopt;
    }
    return fromHex(hex);
}

// The hex hash the server stores of this installation's key for it: SHA-256
// of HMAC-SHA256(key, server host). Empty without a key.
std::string getKeyHash(const std::string& server) {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_key_hashes.find(server); it != g_key_hashes.end())
        return it->second;

    const auto key = loadInstallationKey();
    if (!key)
        return {};
    const auto server_key =
        toHex(hmacSha256(*key, asBytes(toLowerAscii(server))));
    const auto key_hash = toHex(sha256(asBytes(server_key)));
    g_key_hashes.emplace(server, key_hash);
    return key_hash;
}

u32 resolveIpv4(const std::string& host) {
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) == 1)
        return address.s_addr;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return 0;
    const u32 ip =
        reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);
    return ip;
}

// A request of Hydra's own to the server's HTTP port: the status (0 when the
// server could not be reached) and the body
std::pair<i32, std::string> requestServer(const Config& config,
                                          std::string_view method,
                                          const std::string& target,
                                          std::string_view extra_headers) {
    const u32 address = getServerAddress(config);
    if (address == 0)
        return {0, {}};

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {0, {}};
    timeval timeout{.tv_sec = 4, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.http_port);
    addr.sin_addr.s_addr = address;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        ::close(fd);
        return {0, {}};
    }

    auto request =
        fmt::format("{} {} HTTP/1.1\r\nHost: {}:{}\r\nConnection: "
                    "close\r\nContent-Length: 0\r\n{}",
                    method, target, config.server, config.http_port,
                    extra_headers);
    if (const auto proof = makeProof(config, method, target); !proof.empty())
        request += fmt::format("X-RyuNET-Proof: {}\r\n", proof);
    request += "\r\n";
    if (::send(fd, request.data(), request.size(), SEND_FLAGS) !=
        static_cast<ssize_t>(request.size())) {
        ::close(fd);
        return {0, {}};
    }

    std::string reply;
    char buffer[4096];
    for (ssize_t n; reply.size() < 0x10000 &&
                    (n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0;)
        reply.append(buffer, static_cast<usize>(n));
    ::close(fd);

    i32 status = 0;
    if (reply.starts_with("HTTP/") && reply.size() > 12)
        status = std::atoi(reply.c_str() + 9);
    const auto body_start = reply.find("\r\n\r\n");
    return {status, body_start == std::string::npos
                        ? std::string{}
                        : reply.substr(body_start + 4)};
}

std::string urlEncode(std::string_view str) {
    std::string encoded;
    for (const auto c : str) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            encoded += c;
        else
            encoded += fmt::format("%{:02X}", static_cast<u8>(c));
    }
    return encoded;
}

// Registers this installation for a free nickname before the title logs in
// with it (OCTR's claim_nick): the title's own requests cannot register
void claimNickname(const Config& config) {
    std::thread([config] {
        const auto key_hash = getKeyHash(config.server);
        if (key_hash.empty())
            return;

        const auto query = "name=" + urlEncode(config.nickname);
        const auto [status, body] =
            requestServer(config, "GET", "/ryunet/nick?" + query, "");
        if (status == 0) {
            LOG_WARN(Services, "Online: {}:{} did not answer", config.server,
                     config.http_port);
            return;
        }
        if (body.find("\"free\"") == std::string::npos) {
            LOG_INFO(Services, "Online: nickname {}: HTTP {} {}",
                     config.nickname, status, body.substr(0, 200));
            return;
        }

        const auto [claim_status, claim_body] = requestServer(
            config, "POST", "/ryunet/nick/claim?" + query,
            fmt::format("X-RyuNET-Key-Hash: {}\r\n", key_hash));
        LOG_INFO(Services, "Online: nickname {} claimed: HTTP {} {}",
                 config.nickname, claim_status, claim_body.substr(0, 200));
    }).detach();
}

// The Demonware SDK checks the auth replies against an RSA-2048 public key
// (DER) in the title's read-only data. OCTR patches it in the PS4 executable
// at a fixed offset; here the key is found by its DER framing, and patched
// only when there is exactly one.
void patchDemonwareKey(const Config& config, std::span<u8> rodata) {
    static constexpr std::array<u8, 9> KEY_HEAD = {
        0x30, 0x82, 0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00};
    static constexpr std::array<u8, 5> KEY_TAIL = {0x02, 0x03, 0x01, 0x00,
                                                   0x01};
    constexpr usize MODULUS_SIZE = 256;
    constexpr usize KEY_SIZE =
        KEY_HEAD.size() + MODULUS_SIZE + KEY_TAIL.size();

    if (config.demonware_key.empty())
        return;
    const auto modulus = fromHex(config.demonware_key);
    if (!modulus || modulus->size() != MODULUS_SIZE) {
        LOG_ERROR(Services,
                  "Online: demonware_key is not a 2048-bit modulus in hex");
        return;
    }

    std::vector<usize> offsets;
    for (usize i = 0; i + KEY_SIZE <= rodata.size(); i++) {
        if (rodata[i] != KEY_HEAD[0])
            continue;
        if (std::memcmp(rodata.data() + i, KEY_HEAD.data(), KEY_HEAD.size()) ==
                0 &&
            std::memcmp(rodata.data() + i + KEY_HEAD.size() + MODULUS_SIZE,
                        KEY_TAIL.data(), KEY_TAIL.size()) == 0)
            offsets.push_back(i);
    }

    if (offsets.size() != 1) {
        std::string list;
        for (const auto offset : offsets)
            list += fmt::format(" {:#x}", offset);
        LOG_WARN(Services,
                 "Online: {} RSA-2048 public keys in the main module's "
                 "read-only data (at{}), the Demonware key is left as is",
                 offsets.size(), list.empty() ? " none" : list);
        return;
    }

    std::memcpy(rodata.data() + offsets[0] + KEY_HEAD.size(), modulus->data(),
                MODULUS_SIZE);
    LOG_INFO(Services,
             "Online: Demonware key at .rodata+{:#x} replaced with the "
             "server's",
             offsets[0]);
}

} // namespace

void loadTitle(u64 title_id, std::span<u8> main_rodata) {
    std::unique_lock lock(g_mutex);
    auto& entry = g_titles[title_id];
    if (entry)
        g_retired.push_back(std::move(entry));
    entry = readConfig(title_id);
    if (!entry)
        return;

    const auto config = *entry;
    lock.unlock();

    patchDemonwareKey(config, main_rodata);
    claimNickname(config);
}

const Config* getConfig(u64 title_id) {
    std::lock_guard lock(g_mutex);
    auto it = g_titles.find(title_id);
    if (it == g_titles.end())
        it = g_titles.emplace(title_id, readConfig(title_id)).first;
    return it->second.get();
}

u32 getServerAddress(const Config& config) {
    {
        std::lock_guard lock(g_mutex);
        if (const auto it = g_server_addresses.find(config.server);
            it != g_server_addresses.end())
            return it->second;
    }

    const u32 address = resolveIpv4(config.server);
    if (address == 0) {
        LOG_ERROR(Services, "Online: server {} does not resolve",
                  config.server);
        return 0;
    }

    std::lock_guard lock(g_mutex);
    g_server_addresses.emplace(config.server, address);
    return address;
}

u32 getLocalAddress(const Config& config) {
    // A UDP socket connected to the server: no packet is sent, the host picks
    // the interface
    const u32 server = getServerAddress(config);
    if (server == 0)
        return 0;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(3074);
    to.sin_addr.s_addr = server;
    u32 address = 0;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
        0) {
        sockaddr_in local{};
        socklen_t size = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0)
            address = local.sin_addr.s_addr;
    }
    ::close(fd);
    return address;
}

bool isRedirectedHost(const Config& config, std::string_view host) {
    const auto lower_host = toLowerAscii(host);
    for (const auto& pattern : config.redirect_hosts) {
        const auto lower_pattern = toLowerAscii(pattern);
        if (lower_pattern.starts_with("*.")) {
            // Any subdomain
            if (lower_host.ends_with(std::string_view(lower_pattern).substr(1)))
                return true;
        } else if (lower_host == lower_pattern) {
            return true;
        }
    }
    return false;
}

u64 getAccountId(const Config& config) {
    // OCTR's runtime_np_calculate_account_id: FNV-1a of the lowercase
    // nickname mixed with its SHA-256
    const auto lower = toLowerAscii(config.nickname).substr(0, 127);
    u64 hash = 14695981039346656037ull;
    for (const auto c : lower) {
        hash ^= static_cast<u8>(c);
        hash *= 1099511628211ull;
    }
    const auto digest = sha256(asBytes(lower));
    u64 digest_head;
    std::memcpy(&digest_head, digest.data(), sizeof(digest_head));
    return 1810000000ull + ((digest_head ^ hash) % 80000000ull);
}

std::string makeProof(const Config& config, std::string_view method,
                      std::string_view target) {
    // The server's nick_proof.go: "1.<time>.<nonce>.<mac>", the MAC an
    // HMAC-SHA256 keyed with the key hash (as hex text) of the time, the
    // nonce, the method and the request target
    const auto key_hash = getKeyHash(config.server);
    if (key_hash.empty())
        return {};

    const auto time = static_cast<i64>(std::time(nullptr));
    const auto nonce = randomHex(16);
    const auto message =
        fmt::format("{}\n{}\n{}\n{}", time, nonce, method, target);
    return fmt::format("1.{}.{}.{}", time, nonce,
                       toHex(hmacSha256(asBytes(key_hash), asBytes(message))));
}

std::string HttpProofInjector::feed(std::span<const u8> data) {
    constexpr usize MAX_HEAD_SIZE = 0x4000;

    std::string out;
    usize pos = 0;
    while (pos < data.size()) {
        const auto rest = data.subspan(pos);
        if (raw) {
            out.append(reinterpret_cast<const char*>(rest.data()), rest.size());
            break;
        }

        if (body_remaining != 0) {
            const auto size =
                static_cast<usize>(std::min<u64>(body_remaining, rest.size()));
            out.append(reinterpret_cast<const char*>(rest.data()), size);
            body_remaining -= size;
            pos += size;
            continue;
        }

        // Request head
        const auto old_size = head.size();
        head.append(reinterpret_cast<const char*>(rest.data()), rest.size());
        const auto end =
            head.find("\r\n\r\n", old_size >= 3 ? old_size - 3 : 0);
        if (end == std::string::npos) {
            if (head.size() > MAX_HEAD_SIZE) {
                out += head;
                head.clear();
                raw = true;
            }
            break;
        }
        const auto head_size = end + 4;
        pos = data.size() - (head.size() - head_size);
        head.resize(head_size);

        // Request line and the body's framing
        const std::string_view head_view(head);
        const auto line = head_view.substr(0, head_view.find("\r\n"));
        const auto method_end = line.find(' ');
        const auto target_end = line.find(' ', method_end + 1);
        if (method_end == std::string_view::npos ||
            target_end == std::string_view::npos) {
            out += head;
            head.clear();
            raw = true;
            continue;
        }
        const auto method = line.substr(0, method_end);
        const auto target =
            line.substr(method_end + 1, target_end - method_end - 1);

        const auto lower_head = toLowerAscii(head_view);
        if (const auto at = lower_head.find("\r\ncontent-length:");
            at != std::string::npos)
            body_remaining = std::strtoull(
                lower_head.c_str() + at + sizeof("\r\ncontent-length:") - 1,
                nullptr, 10);
        if (lower_head.find("\r\ntransfer-encoding:") != std::string::npos)
            raw = true; // Chunked: the next head cannot be found

        LOG_INFO(Services, "Online: HTTP request {}", line);
        if (const auto proof = makeProof(config, method, target);
            !proof.empty()) {
            out.append(head, 0, head_size - 2);
            out += fmt::format("X-RyuNET-Proof: {}\r\n\r\n", proof);
        } else {
            out += head;
        }
        head.clear();
    }

    return out;
}

} // namespace hydra::horizon::online

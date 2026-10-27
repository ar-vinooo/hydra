#include "core/horizon/services/socket/resolver/resolver.hpp"
#include "core/horizon/services/socket/resolver/address_serialization.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdlib>

#include "core/horizon/kernel/process.hpp"
#include "core/horizon/online/online.hpp"

namespace hydra::horizon::services::socket::resolver {

DEFINE_SERVICE_COMMAND_TABLE(IResolver, 2, getHostByNameRequest, 6,
                             getAddrInfoRequest, 10,
                             getHostByNameRequestWithOptions, 12,
                             getAddrInfoRequestWithOptions)

namespace {

// nn::socket uses Linux errno values and FreeBSD's netdb values
constexpr u32 EAGAIN_ = 11;
constexpr i32 EAI_AGAIN_ = 2;
constexpr i32 EAI_NONAME_ = 8;
constexpr u32 HOST_NOT_FOUND_ = 1;
constexpr u32 TRY_AGAIN_ = 2;
constexpr u32 NO_RECOVERY_ = 3;
constexpr u32 GUEST_AF_INET = 2;
constexpr u32 GUEST_SOCK_STREAM = 1;
constexpr u32 GUEST_SOCK_DGRAM = 2;
constexpr u32 ADDRINFO_MAGIC = 0xbeefcafe;

struct LookupResult {
    i32 ret;         // getaddrinfo error
    u32 host_error;  // h_errno
    u32 error;       // errno
    u32 size;        // Serialized size
};

template <typename Buffer>
std::span<u8> getSpan(Buffer& buffer) {
    if (!buffer.stream)
        return {};
    return {buffer.stream->getPtr(),
            static_cast<usize>(buffer.stream->getSize())};
}

template <typename Buffer>
std::string readString(Buffer& buffer) {
    const auto span = getSpan(buffer);
    const auto end = std::find(span.begin(), span.end(), u8{0});
    return std::string(span.begin(), end);
}

u32 readBe32(std::span<const u8> data, usize offset) {
    return (u32(data[offset]) << 24) | (u32(data[offset + 1]) << 16) |
           (u32(data[offset + 2]) << 8) | u32(data[offset + 3]);
}

void appendBe16(std::vector<u8>& data, u16 value) {
    data.push_back(static_cast<u8>(value >> 8));
    data.push_back(static_cast<u8>(value));
}

void appendBe32(std::vector<u8>& data, u32 value) {
    appendBe16(data, static_cast<u16>(value >> 16));
    appendBe16(data, static_cast<u16>(value));
}

void appendLe32(std::vector<u8>& data, u32 value) {
    for (u32 i = 0; i < 4; i++)
        data.push_back(static_cast<u8>(value >> (i * 8)));
}

std::string toString(u32 address) {
    char ip[INET_ADDRSTRLEN] = {};
    in_addr addr{.s_addr = address};
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    return ip;
}

// IPv4 addresses (network byte order) for a host: the server's for the
// redirected hosts. Other names are reported as not found, so that nothing
// reaches the official servers (as OCTR's host whitelist).
std::vector<u32> resolve(const online::Config& config,
                         const std::string& host) {
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1)
        return {literal.s_addr};

    if (!online::isRedirectedHost(config, host)) {
        LOG_INFO(Services, "Online: DNS {} -> not found (not redirected)",
                 host);
        return {};
    }

    const u32 server = online::getServerAddress(config);
    if (server == 0)
        return {};
    LOG_INFO(Services, "Online: DNS {} -> {} (server)", host,
             toString(server));
    return {server};
}

LookupResult getAddrInfo(kernel::Process* process, const std::string& host,
                         const std::string& service,
                         std::span<const u8> hints, std::span<u8> out) {
    const auto* config = online::getConfig(process->getTitleId());
    if (!config) {
        // There is no network
        return {EAI_AGAIN_, TRY_AGAIN_, EAGAIN_, 0};
    }

    u32 socket_type = 0;
    u32 protocol = 0;
    if (hints.size() >= 20 && readBe32(hints, 0) == ADDRINFO_MAGIC) {
        socket_type = readBe32(hints, 12);
        protocol = readBe32(hints, 16);
    }
    if (socket_type == 0)
        socket_type = GUEST_SOCK_STREAM;
    if (protocol == 0)
        protocol = socket_type == GUEST_SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;

    u16 port = 0;
    if (service == "http")
        port = 80;
    else if (service == "https")
        port = 443;
    else if (!service.empty())
        port = static_cast<u16>(std::strtoul(service.c_str(), nullptr, 10));

    const auto addresses = resolve(*config, host);
    if (addresses.empty())
        return {EAI_NONAME_, HOST_NOT_FOUND_, 0, 0};

    // Serialized addrinfo list (libnx's resolver.c): big endian fields, the
    // sockaddr_in with a 16-bit family, a canonical name, a zero terminator
    std::vector<u8> data;
    for (const auto address : addresses) {
        std::vector<u8> entry;
        appendBe32(entry, ADDRINFO_MAGIC);
        appendBe32(entry, 0); // ai_flags
        appendBe32(entry, GUEST_AF_INET);
        appendBe32(entry, socket_type);
        appendBe32(entry, protocol);
        appendBe32(entry, 16); // ai_addrlen
        const auto sockaddr = serializeIpv4SockAddr(address, htons(port));
        entry.insert(entry.end(), sockaddr.begin(), sockaddr.end());
        entry.push_back(0);                // ai_canonname
        if (data.size() + entry.size() + 4 > out.size())
            break;
        data.insert(data.end(), entry.begin(), entry.end());
    }
    data.resize(data.size() + 4, 0);
    if (data.size() > out.size())
        return {EAI_NONAME_, HOST_NOT_FOUND_, 0, 0};

    std::copy(data.begin(), data.end(), out.begin());
    return {0, 0, 0, static_cast<u32>(data.size())};
}

LookupResult getHostByName(kernel::Process* process, const std::string& host,
                           std::span<u8> out) {
    const auto* config = online::getConfig(process->getTitleId());
    if (!config) {
        // There is no network
        return {EAI_AGAIN_, TRY_AGAIN_, EAGAIN_, 0};
    }

    const auto addresses = resolve(*config, host);
    if (addresses.empty())
        return {EAI_NONAME_, HOST_NOT_FOUND_, 0, 0};

    // Serialized hostent (as yuzu): the name, no aliases, the type and length
    // big endian, then the addresses, which nn::socket swaps once more
    std::vector<u8> data(host.begin(), host.end());
    data.push_back(0);
    appendBe32(data, 0); // Alias count
    appendBe16(data, GUEST_AF_INET);
    appendBe16(data, 4);
    appendBe32(data, static_cast<u32>(addresses.size()));
    for (const auto address : addresses)
        appendLe32(data, ntohl(address));
    if (data.size() > out.size())
        return {EAI_NONAME_, NO_RECOVERY_, 0, 0};

    std::copy(data.begin(), data.end(), out.begin());
    return {0, 0, 0, static_cast<u32>(data.size())};
}

} // namespace

result_t
IResolver::getHostByNameRequest(kernel::Process* process,
                                InBuffer<BufferAttr::MapAlias> name_buffer,
                                u32* out_host_error, u32* out_errno,
                                u32* out_size,
                                OutBuffer<BufferAttr::MapAlias> out_buffer) {
    const auto result =
        getHostByName(process, readString(name_buffer), getSpan(out_buffer));
    *out_host_error = result.host_error;
    *out_errno = result.error;
    *out_size = result.size;
    return RESULT_SUCCESS;
}

// Command 6's output order follows libnx's sfdnsres.c
result_t
IResolver::getAddrInfoRequest(kernel::Process* process,
                              InBuffer<BufferAttr::MapAlias> node_buffer,
                              InBuffer<BufferAttr::MapAlias> service_buffer,
                              InBuffer<BufferAttr::MapAlias> hints_buffer,
                              u32* out_errno, i32* out_ret, u32* out_size,
                              OutBuffer<BufferAttr::MapAlias> out_buffer) {
    const auto result =
        getAddrInfo(process, readString(node_buffer),
                    readString(service_buffer), getSpan(hints_buffer),
                    getSpan(out_buffer));
    *out_errno = result.error;
    *out_ret = result.ret;
    *out_size = result.size;
    return RESULT_SUCCESS;
}

// Commands 10 and 12's output order follows Atmosphere's dns_mitm IResolver
// interface
result_t IResolver::getHostByNameRequestWithOptions(
    kernel::Process* process, InBuffer<BufferAttr::AutoSelect> name_buffer,
    InBuffer<BufferAttr::AutoSelect> options_buffer, u32* out_size,
    i32* out_host_error, i32* out_errno,
    OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    (void)options_buffer;
    const auto result =
        getHostByName(process, readString(name_buffer), getSpan(out_buffer));
    *out_size = result.size;
    *out_host_error = static_cast<i32>(result.host_error);
    *out_errno = static_cast<i32>(result.error);
    return RESULT_SUCCESS;
}

result_t IResolver::getAddrInfoRequestWithOptions(
    kernel::Process* process, InBuffer<BufferAttr::AutoSelect> node_buffer,
    InBuffer<BufferAttr::AutoSelect> service_buffer,
    InBuffer<BufferAttr::AutoSelect> hints_buffer,
    InBuffer<BufferAttr::AutoSelect> options_buffer, u32* out_size,
    i32* out_ret, i32* out_host_error, i32* out_errno,
    OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    (void)options_buffer;
    const auto result =
        getAddrInfo(process, readString(node_buffer),
                    readString(service_buffer), getSpan(hints_buffer),
                    getSpan(out_buffer));
    *out_size = result.size;
    *out_ret = result.ret;
    *out_host_error = static_cast<i32>(result.host_error);
    *out_errno = static_cast<i32>(result.error);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::socket::resolver

#include "core/horizon/services/socket/host_sockets.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <vector>

namespace hydra::horizon::services::socket {

namespace {

// nn::socket constants: FreeBSD values, except errno (Linux values) and
// O_NONBLOCK
constexpr u32 GUEST_AF_INET = 2;
constexpr u32 GUEST_SOCK_STREAM = 1;
constexpr u32 GUEST_SOCK_DGRAM = 2;
constexpr u32 GUEST_SOCK_NONBLOCK = 0x20000000;
constexpr i32 GUEST_O_NONBLOCK = 0x800;
constexpr i32 GUEST_F_GETFL = 3;
constexpr i32 GUEST_F_SETFL = 4;
constexpr i32 GUEST_MSG_OOB = 0x1;
constexpr i32 GUEST_MSG_PEEK = 0x2;
constexpr i32 GUEST_MSG_DONTWAIT = 0x80;
constexpr i32 GUEST_SOL_SOCKET = 0xffff;
constexpr i32 GUEST_SO_SNDTIMEO = 0x1005;
constexpr i32 GUEST_SO_RCVTIMEO = 0x1006;
constexpr i32 GUEST_SO_ERROR = 0x1007;
constexpr i32 GUEST_IPPROTO_IP = 0;
constexpr i32 GUEST_IPPROTO_TCP = 6;
constexpr u32 GUEST_FIONREAD = 0x4004667f;
constexpr u32 GUEST_FIONBIO = 0x8004667e;
constexpr i16 GUEST_POLLIN = 0x1;
constexpr i16 GUEST_POLLPRI = 0x2;
constexpr i16 GUEST_POLLOUT = 0x4;
constexpr i16 GUEST_POLLERR = 0x8;
constexpr i16 GUEST_POLLHUP = 0x10;
constexpr i16 GUEST_POLLNVAL = 0x20;
constexpr i16 GUEST_POLLRDNORM = 0x40;
constexpr i16 GUEST_POLLWRNORM = GUEST_POLLOUT;

constexpr u32 E_INTR = 4;
constexpr u32 E_BADF = 9;
constexpr u32 E_AGAIN = 11;
constexpr u32 E_INVAL = 22;
constexpr u32 E_PROTONOSUPPORT = 93;
constexpr u32 E_AFNOSUPPORT = 97;
constexpr u32 E_TIMEDOUT = 110;
constexpr u32 E_INPROGRESS = 115;

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

// nn::socket's sockaddr_in
struct GuestSockAddrIn {
    u8 len;
    u8 family;
    u16 port; // Network byte order
    u32 addr; // Network byte order
    u8 zero[8];
};
static_assert(sizeof(GuestSockAddrIn) == 16);

struct GuestPollFd {
    i32 fd;
    i16 events;
    i16 revents;
};
static_assert(sizeof(GuestPollFd) == 8);

u32 translateErrno(int error) {
    static const std::pair<int, u32> TABLE[] = {
        {EPERM, 1},
        {ENOENT, 2},
        {EINTR, 4},
        {EIO, 5},
        {EBADF, 9},
        {EAGAIN, 11},
        {EWOULDBLOCK, 11},
        {ENOMEM, 12},
        {EACCES, 13},
        {EFAULT, 14},
        {EBUSY, 16},
        {EEXIST, 17},
        {EINVAL, 22},
        {ENFILE, 23},
        {EMFILE, 24},
        {ENOSPC, 28},
        {EPIPE, 32},
        {ENOTSOCK, 88},
        {EDESTADDRREQ, 89},
        {EMSGSIZE, 90},
        {EPROTOTYPE, 91},
        {ENOPROTOOPT, 92},
        {EPROTONOSUPPORT, 93},
        {EOPNOTSUPP, 95},
        {ENOTSUP, 95},
        {EAFNOSUPPORT, 97},
        {EADDRINUSE, 98},
        {EADDRNOTAVAIL, 99},
        {ENETDOWN, 100},
        {ENETUNREACH, 101},
        {ENETRESET, 102},
        {ECONNABORTED, 103},
        {ECONNRESET, 104},
        {ENOBUFS, 105},
        {EISCONN, 106},
        {ENOTCONN, 107},
        {ESHUTDOWN, 108},
        {ETIMEDOUT, 110},
        {ECONNREFUSED, 111},
        {EHOSTDOWN, 112},
        {EHOSTUNREACH, 113},
        {EALREADY, 114},
        {EINPROGRESS, 115},
    };
    for (const auto& [host, guest] : TABLE) {
        if (host == error)
            return guest;
    }
    return E_INVAL;
}

std::optional<sockaddr_in> toHostAddr(std::span<const u8> guest) {
    GuestSockAddrIn addr{};
    if (guest.size() < offsetof(GuestSockAddrIn, zero))
        return std::nullopt;
    std::memcpy(&addr, guest.data(), std::min(guest.size(), sizeof(addr)));
    if (addr.family != GUEST_AF_INET)
        return std::nullopt;

    sockaddr_in host{};
#ifdef __APPLE__
    host.sin_len = sizeof(host);
#endif
    host.sin_family = AF_INET;
    host.sin_port = addr.port;
    host.sin_addr.s_addr = addr.addr;
    return host;
}

void writeGuestAddr(const sockaddr_in& host, std::span<u8> out,
                    u32& out_size) {
    const GuestSockAddrIn addr{
        .len = sizeof(GuestSockAddrIn),
        .family = GUEST_AF_INET,
        .port = host.sin_port,
        .addr = host.sin_addr.s_addr,
        .zero = {},
    };
    const auto size = std::min(out.size(), sizeof(addr));
    std::memcpy(out.data(), &addr, size);
    out_size = static_cast<u32>(sizeof(addr));
}

std::string toString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return fmt::format("{}:{}", ip, ntohs(addr.sin_port));
}

void makeHostSocketNonBlocking(int host_fd) {
    ::fcntl(host_fd, F_SETFL, ::fcntl(host_fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(host_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

// An nn::socket option, as the host's level and name
std::optional<std::pair<int, int>> translateOption(i32 level, i32 name) {
    switch (level) {
    case GUEST_SOL_SOCKET:
        switch (name) {
        case 0x4:
            return std::pair{SOL_SOCKET, SO_REUSEADDR};
        case 0x8:
            return std::pair{SOL_SOCKET, SO_KEEPALIVE};
        case 0x20:
            return std::pair{SOL_SOCKET, SO_BROADCAST};
        case 0x80:
            return std::pair{SOL_SOCKET, SO_LINGER};
        case 0x100:
            return std::pair{SOL_SOCKET, SO_OOBINLINE};
        case 0x200:
            return std::pair{SOL_SOCKET, SO_REUSEPORT};
        case 0x1001:
            return std::pair{SOL_SOCKET, SO_SNDBUF};
        case 0x1002:
            return std::pair{SOL_SOCKET, SO_RCVBUF};
        case 0x1003:
            return std::pair{SOL_SOCKET, SO_SNDLOWAT};
        case 0x1004:
            return std::pair{SOL_SOCKET, SO_RCVLOWAT};
        case GUEST_SO_ERROR:
            return std::pair{SOL_SOCKET, SO_ERROR};
        case 0x1008:
            return std::pair{SOL_SOCKET, SO_TYPE};
        default:
            break;
        }
        break;
    case GUEST_IPPROTO_TCP:
        if (name == 0x1)
            return std::pair{IPPROTO_TCP, TCP_NODELAY};
        break;
    case GUEST_IPPROTO_IP:
        switch (name) {
        case 3:
            return std::pair{IPPROTO_IP, IP_TOS};
        case 4:
            return std::pair{IPPROTO_IP, IP_TTL};
        case 9:
            return std::pair{IPPROTO_IP, IP_MULTICAST_IF};
        case 10:
            return std::pair{IPPROTO_IP, IP_MULTICAST_TTL};
        case 11:
            return std::pair{IPPROTO_IP, IP_MULTICAST_LOOP};
        case 12:
            return std::pair{IPPROTO_IP, IP_ADD_MEMBERSHIP};
        case 13:
            return std::pair{IPPROTO_IP, IP_DROP_MEMBERSHIP};
        default:
            break;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

short toHostPollEvents(i16 events) {
    short host = 0;
    if (events & (GUEST_POLLIN | GUEST_POLLRDNORM))
        host |= POLLIN;
    if (events & GUEST_POLLPRI)
        host |= POLLPRI;
    if (events & GUEST_POLLOUT)
        host |= POLLOUT;
    return host;
}

i16 toGuestPollEvents(short host, i16 requested) {
    i16 events = 0;
    if (host & POLLIN) {
        const i16 read = requested & (GUEST_POLLIN | GUEST_POLLRDNORM);
        events |= read != 0 ? read : GUEST_POLLIN;
    }
    if (host & POLLPRI)
        events |= GUEST_POLLPRI;
    if (host & POLLOUT)
        events |= GUEST_POLLWRNORM;
    if (host & POLLERR)
        events |= GUEST_POLLERR;
    if (host & POLLHUP)
        events |= GUEST_POLLHUP;
    if (host & POLLNVAL)
        events |= GUEST_POLLNVAL;
    return events;
}

int toPollTimeout(i64 timeout_ms) {
    return timeout_ms < 0 ? -1 : static_cast<int>(std::min<i64>(timeout_ms, INT_MAX));
}

} // namespace

HostSockets::HostSockets() {
    if (::pipe(wake_pipe.data()) != 0)
        LOG_ERROR(Services, "Online: failed to create the socket wake pipe");
}

HostSockets::~HostSockets() {
    closeAll();
    for (const auto fd : wake_pipe) {
        if (fd >= 0)
            ::close(fd);
    }
}

void HostSockets::interrupt() {
    if (interrupted.exchange(true))
        return;
    if (wake_pipe[1] >= 0) {
        const u8 byte = 0;
        [[maybe_unused]] const auto written = ::write(wake_pipe[1], &byte, 1);
    }
}

void HostSockets::closeAll() {
    for (const auto& [fd, socket] : sockets)
        ::close(socket.host_fd);
    sockets.clear();
}

HostSockets::Socket* HostSockets::get(i32 fd) {
    const auto it = sockets.find(fd);
    return it == sockets.end() ? nullptr : &it->second;
}

i32 HostSockets::add(Socket socket) {
    const auto fd = next_fd++;
    sockets.emplace(fd, std::move(socket));
    return fd;
}

CallResult HostSockets::wait(const Socket& socket, short events,
                             i64 timeout_ms) {
    std::array<pollfd, 2> fds = {pollfd{socket.host_fd, events, 0},
                                 pollfd{wake_pipe[0], POLLIN, 0}};
    const int count =
        ::poll(fds.data(), fds.size(), toPollTimeout(timeout_ms));
    if (interrupted || fds[1].revents != 0)
        return {-1, E_INTR};
    if (count < 0)
        return {-1, translateErrno(errno)};
    if (count == 0)
        return {-1, E_AGAIN}; // Timed out
    return {0, 0};
}

template <typename Op>
CallResult HostSockets::run(const Socket& socket, short events, bool may_wait,
                            i64 timeout_ms, Op op) {
    while (true) {
        const auto ret = op();
        if (ret >= 0)
            return {static_cast<i32>(ret), 0};
        const int error = errno;
        if (error != EAGAIN && error != EWOULDBLOCK)
            return {-1, translateErrno(error)};
        if (!may_wait)
            return {-1, E_AGAIN};
        if (const auto result = wait(socket, events, timeout_ms);
            result.ret < 0)
            return result;
    }
}

CallResult HostSockets::sendAll(Socket& socket, std::span<const u8> data) {
    // Requests to the server are small: they go out whole, whether the title
    // made the socket blocking or not
    constexpr i64 TIMEOUT_MS = 10000;

    usize sent = 0;
    while (sent < data.size()) {
        const auto ret = ::send(socket.host_fd, data.data() + sent,
                                data.size() - sent, SEND_FLAGS);
        if (ret >= 0) {
            sent += static_cast<usize>(ret);
            continue;
        }
        const int error = errno;
        if (error != EAGAIN && error != EWOULDBLOCK)
            return {-1, translateErrno(error)};
        if (const auto result = wait(socket, POLLOUT, TIMEOUT_MS);
            result.ret < 0)
            return result;
    }
    return {static_cast<i32>(sent), 0};
}

CallResult HostSockets::open(const online::Config& config, u32 domain,
                             u32 type, u32 protocol) {
    const u32 base_type = type & 0xffff;
    if (domain != GUEST_AF_INET) {
        LOG_WARN(Services, "Online: socket domain {} is not supported",
                 domain);
        return {-1, E_AFNOSUPPORT};
    }
    if (base_type != GUEST_SOCK_STREAM && base_type != GUEST_SOCK_DGRAM) {
        LOG_WARN(Services, "Online: socket type {:#x} is not supported", type);
        return {-1, E_PROTONOSUPPORT};
    }

    const bool is_stream = base_type == GUEST_SOCK_STREAM;
    const int host_fd = ::socket(AF_INET, is_stream ? SOCK_STREAM : SOCK_DGRAM,
                                 static_cast<int>(protocol));
    if (host_fd < 0)
        return {-1, translateErrno(errno)};
    makeHostSocketNonBlocking(host_fd);

    const auto fd = add(Socket{
        .host_fd = host_fd,
        .is_stream = is_stream,
        .non_blocking = (type & GUEST_SOCK_NONBLOCK) != 0,
        .config = &config,
    });
    LOG_DEBUG(Services, "Online: socket {} ({})", fd,
              is_stream ? "TCP" : "UDP");
    return {fd, 0};
}

CallResult HostSockets::close(i32 fd) {
    const auto it = sockets.find(fd);
    if (it == sockets.end())
        return {-1, E_BADF};
    ::close(it->second.host_fd);
    sockets.erase(it);
    return {0, 0};
}

CallResult HostSockets::bind(i32 fd, std::span<const u8> addr) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    const auto host_addr = toHostAddr(addr);
    if (!host_addr)
        return {-1, E_AFNOSUPPORT};

    LOG_INFO(Services, "Online: socket {} ({}) bound to {}", fd,
             socket->is_stream ? "TCP" : "UDP", toString(*host_addr));
    if (::bind(socket->host_fd, reinterpret_cast<const sockaddr*>(&*host_addr),
               sizeof(sockaddr_in)) != 0)
        return {-1, translateErrno(errno)};
    return {0, 0};
}

CallResult HostSockets::connect(i32 fd, std::span<const u8> addr) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    auto host_addr = toHostAddr(addr);
    if (!host_addr)
        return {-1, E_AFNOSUPPORT};

    // The server's ports 80 and 443 lead to its HTTP port, where TLS is not
    // spoken (OCTR's sceHttp turns the HTTPS requests to RyuNET into HTTP)
    const auto requested = toString(*host_addr);
    if (socket->is_stream && socket->config) {
        const u32 server = online::getServerAddress(*socket->config);
        const u16 port = ntohs(host_addr->sin_port);
        if (server != 0 && host_addr->sin_addr.s_addr == server &&
            (port == 80 || port == 443)) {
            host_addr->sin_port = htons(socket->config->http_port);
            socket->http.emplace(*socket->config);
        }
    }
    LOG_INFO(Services, "Online: socket {} ({}) connects to {}{}", fd,
             socket->is_stream ? "TCP" : "UDP", requested,
             socket->http ? fmt::format(" (server HTTP port {})",
                                        ntohs(host_addr->sin_port))
                          : std::string{});

    if (::connect(socket->host_fd,
                  reinterpret_cast<const sockaddr*>(&*host_addr),
                  sizeof(sockaddr_in)) == 0)
        return {0, 0};
    const int error = errno;
    if (error != EINPROGRESS)
        return {-1, translateErrno(error)};
    if (socket->non_blocking)
        return {-1, E_INPROGRESS};

    if (const auto result = wait(*socket, POLLOUT, socket->send_timeout_ms);
        result.ret < 0)
        return result.error == E_AGAIN ? CallResult{-1, E_TIMEDOUT} : result;
    int so_error = 0;
    socklen_t size = sizeof(so_error);
    ::getsockopt(socket->host_fd, SOL_SOCKET, SO_ERROR, &so_error, &size);
    if (so_error != 0)
        return {-1, translateErrno(so_error)};
    return {0, 0};
}

CallResult HostSockets::listen(i32 fd, i32 backlog) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    if (::listen(socket->host_fd, backlog) != 0)
        return {-1, translateErrno(errno)};
    return {0, 0};
}

CallResult HostSockets::accept(i32 fd, std::span<u8> out_addr,
                               u32& out_addr_size) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};

    sockaddr_in addr{};
    socklen_t addr_size = sizeof(addr);
    const auto result = run(
        *socket, POLLIN, !socket->non_blocking, socket->recv_timeout_ms, [&] {
            return ::accept(socket->host_fd, reinterpret_cast<sockaddr*>(&addr),
                            &addr_size);
        });
    if (result.ret < 0)
        return result;

    makeHostSocketNonBlocking(result.ret);
    const auto new_fd = add(Socket{
        .host_fd = result.ret,
        .is_stream = true,
        .non_blocking = socket->non_blocking,
        .config = socket->config,
    });
    LOG_INFO(Services, "Online: socket {} accepted {} as socket {}", fd,
             toString(addr), new_fd);
    writeGuestAddr(addr, out_addr, out_addr_size);
    return {new_fd, 0};
}

CallResult HostSockets::recv(i32 fd, std::span<u8> buffer, i32 flags,
                             std::span<u8> out_addr, u32* out_addr_size,
                             bool force_wait) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};

    const int host_flags = ((flags & GUEST_MSG_PEEK) ? MSG_PEEK : 0) |
                           ((flags & GUEST_MSG_OOB) ? MSG_OOB : 0);
    const bool may_wait = (flags & GUEST_MSG_DONTWAIT) == 0 &&
                          (!socket->non_blocking || force_wait);
    sockaddr_in addr{};
    socklen_t addr_size = sizeof(addr);
    const auto result = run(*socket, POLLIN, may_wait,
                            socket->recv_timeout_ms, [&] {
                                return ::recvfrom(
                                    socket->host_fd, buffer.data(),
                                    buffer.size(), host_flags,
                                    reinterpret_cast<sockaddr*>(&addr),
                                    &addr_size);
                            });

    // The server's answers to the title's HTTP requests
    if (socket->http && result.ret > 5 &&
        std::memcmp(buffer.data(), "HTTP/", 5) == 0) {
        const std::string_view reply(reinterpret_cast<const char*>(buffer.data()),
                                     static_cast<usize>(result.ret));
        LOG_INFO(Services, "Online: HTTP reply {}",
                 reply.substr(0, std::min(reply.find("\r\n"), usize{100})));
    }

    if (out_addr_size) {
        *out_addr_size = 0;
        if (result.ret >= 0 && addr_size >= sizeof(sockaddr_in) &&
            addr.sin_family == AF_INET)
            writeGuestAddr(addr, out_addr, *out_addr_size);
    }
    return result;
}

CallResult HostSockets::send(i32 fd, std::span<const u8> buffer, i32 flags,
                             std::span<const u8> addr, bool force_wait) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};

    if (socket->http) {
        const auto data = socket->http->feed(buffer);
        const auto result = sendAll(
            *socket, std::span(reinterpret_cast<const u8*>(data.data()),
                               data.size()));
        if (result.ret < 0)
            return result;
        return {static_cast<i32>(buffer.size()), 0};
    }

    std::optional<sockaddr_in> host_addr;
    if (!addr.empty()) {
        host_addr = toHostAddr(addr);
        if (!host_addr)
            return {-1, E_INVAL};
        const u64 key = (u64(host_addr->sin_addr.s_addr) << 16) |
                        host_addr->sin_port;
        if (logged_destinations.insert(key).second)
            LOG_INFO(Services, "Online: socket {} ({}) sends to {}", fd,
                     socket->is_stream ? "TCP" : "UDP",
                     toString(*host_addr));
    }

    const int host_flags = ((flags & GUEST_MSG_OOB) ? MSG_OOB : 0) | SEND_FLAGS;
    const bool may_wait = (flags & GUEST_MSG_DONTWAIT) == 0 &&
                          (!socket->non_blocking || force_wait);
    return run(*socket, POLLOUT, may_wait, socket->send_timeout_ms, [&] {
                   return host_addr
                              ? ::sendto(socket->host_fd, buffer.data(),
                                         buffer.size(), host_flags,
                                         reinterpret_cast<const sockaddr*>(
                                             &*host_addr),
                                         sizeof(sockaddr_in))
                              : ::send(socket->host_fd, buffer.data(),
                                       buffer.size(), host_flags);
               });
}

CallResult HostSockets::getSockName(i32 fd, std::span<u8> out_addr,
                                    u32& out_addr_size) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    sockaddr_in addr{};
    socklen_t addr_size = sizeof(addr);
    if (::getsockname(socket->host_fd, reinterpret_cast<sockaddr*>(&addr),
                      &addr_size) != 0)
        return {-1, translateErrno(errno)};
    writeGuestAddr(addr, out_addr, out_addr_size);
    return {0, 0};
}

CallResult HostSockets::getPeerName(i32 fd, std::span<u8> out_addr,
                                    u32& out_addr_size) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    sockaddr_in addr{};
    socklen_t addr_size = sizeof(addr);
    if (::getpeername(socket->host_fd, reinterpret_cast<sockaddr*>(&addr),
                      &addr_size) != 0)
        return {-1, translateErrno(errno)};
    writeGuestAddr(addr, out_addr, out_addr_size);
    return {0, 0};
}

CallResult HostSockets::setSockOpt(i32 fd, i32 level, i32 name,
                                   std::span<const u8> value) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};

    // The timeouts apply to the waits here: nn::socket's timeval is two
    // 64-bit values
    if (level == GUEST_SOL_SOCKET &&
        (name == GUEST_SO_RCVTIMEO || name == GUEST_SO_SNDTIMEO)) {
        if (value.size() < 16)
            return {-1, E_INVAL};
        i64 tv[2];
        std::memcpy(tv, value.data(), sizeof(tv));
        const i64 timeout_ms = tv[0] * 1000 + (tv[1] + 999) / 1000;
        (name == GUEST_SO_RCVTIMEO ? socket->recv_timeout_ms
                                   : socket->send_timeout_ms) =
            timeout_ms > 0 ? timeout_ms : -1;
        return {0, 0};
    }

    const auto option = translateOption(level, name);
    if (!option) {
        LOG_DEBUG(Services, "Online: socket option {:#x}:{:#x} ignored", level,
                  name);
        return {0, 0};
    }
    if (::setsockopt(socket->host_fd, option->first, option->second,
                     value.data(), static_cast<socklen_t>(value.size())) != 0)
        return {-1, translateErrno(errno)};
    return {0, 0};
}

CallResult HostSockets::getSockOpt(i32 fd, i32 level, i32 name,
                                   std::span<u8> out_value, u32& out_size) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};

    std::array<u8, 16> value{};
    socklen_t size = sizeof(i32);
    if (level == GUEST_SOL_SOCKET &&
        (name == GUEST_SO_RCVTIMEO || name == GUEST_SO_SNDTIMEO)) {
        const auto timeout_ms = name == GUEST_SO_RCVTIMEO
                                    ? socket->recv_timeout_ms
                                    : socket->send_timeout_ms;
        const i64 tv[2] = {timeout_ms > 0 ? timeout_ms / 1000 : 0,
                           timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0};
        std::memcpy(value.data(), tv, sizeof(tv));
        size = sizeof(tv);
    } else if (const auto option = translateOption(level, name)) {
        size = sizeof(value);
        if (::getsockopt(socket->host_fd, option->first, option->second,
                         value.data(), &size) != 0)
            return {-1, translateErrno(errno)};
        if (level == GUEST_SOL_SOCKET && name == GUEST_SO_ERROR) {
            i32 error;
            std::memcpy(&error, value.data(), sizeof(error));
            error = error != 0 ? static_cast<i32>(translateErrno(error)) : 0;
            std::memcpy(value.data(), &error, sizeof(error));
        }
    } else {
        LOG_DEBUG(Services, "Online: socket option {:#x}:{:#x} reads as 0",
                  level, name);
    }

    const auto copy_size = std::min<usize>(size, out_value.size());
    std::memcpy(out_value.data(), value.data(), copy_size);
    out_size = static_cast<u32>(copy_size);
    return {0, 0};
}

CallResult HostSockets::fcntl(i32 fd, i32 cmd, i32 arg) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    switch (cmd) {
    case GUEST_F_GETFL:
        return {socket->non_blocking ? GUEST_O_NONBLOCK : 0, 0};
    case GUEST_F_SETFL:
        socket->non_blocking = (arg & GUEST_O_NONBLOCK) != 0;
        return {0, 0};
    default:
        LOG_DEBUG(Services, "Online: fcntl {} ignored", cmd);
        return {0, 0};
    }
}

CallResult HostSockets::ioctl(i32 fd, u32 request, std::span<const u8> in,
                              std::span<u8> out) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    switch (request) {
    case GUEST_FIONREAD: {
        int count = 0;
        if (::ioctl(socket->host_fd, FIONREAD, &count) != 0)
            return {-1, translateErrno(errno)};
        const i32 value = count;
        std::memcpy(out.data(), &value, std::min(out.size(), sizeof(value)));
        return {0, 0};
    }
    case GUEST_FIONBIO: {
        i32 value = 0;
        std::memcpy(&value, in.data(), std::min(in.size(), sizeof(value)));
        socket->non_blocking = value != 0;
        return {0, 0};
    }
    default:
        LOG_DEBUG(Services, "Online: ioctl {:#x} ignored", request);
        return {0, 0};
    }
}

CallResult HostSockets::shutdown(i32 fd, i32 how) {
    auto* socket = get(fd);
    if (!socket)
        return {-1, E_BADF};
    if (::shutdown(socket->host_fd, how) != 0)
        return {-1, translateErrno(errno)};
    return {0, 0};
}

CallResult HostSockets::poll(std::span<u8> fds, i32 timeout_ms) {
    const usize count = fds.size() / sizeof(GuestPollFd);
    std::vector<GuestPollFd> guest_fds(count);
    std::memcpy(guest_fds.data(), fds.data(), count * sizeof(GuestPollFd));

    std::vector<pollfd> host_fds(count + 1);
    bool any_invalid = false;
    for (usize i = 0; i < count; i++) {
        const auto* socket = guest_fds[i].fd >= 0 ? get(guest_fds[i].fd) : nullptr;
        any_invalid |= guest_fds[i].fd >= 0 && !socket;
        host_fds[i] = pollfd{socket ? socket->host_fd : -1,
                             toHostPollEvents(guest_fds[i].events), 0};
    }
    host_fds[count] = pollfd{wake_pipe[0], POLLIN, 0};

    const int ret = ::poll(host_fds.data(), host_fds.size(),
                           any_invalid ? 0 : timeout_ms);
    if (interrupted || host_fds[count].revents != 0)
        return {-1, E_INTR};
    if (ret < 0)
        return {-1, translateErrno(errno)};

    i32 ready = 0;
    for (usize i = 0; i < count; i++) {
        auto& guest_fd = guest_fds[i];
        if (guest_fd.fd >= 0 && host_fds[i].fd < 0)
            guest_fd.revents = GUEST_POLLNVAL;
        else
            guest_fd.revents =
                toGuestPollEvents(host_fds[i].revents, guest_fd.events);
        if (guest_fd.revents != 0)
            ready++;
    }
    std::memcpy(fds.data(), guest_fds.data(), count * sizeof(GuestPollFd));
    return {ready, 0};
}

CallResult HostSockets::select(i32 nfds, std::span<u8> read_fds,
                               std::span<u8> write_fds,
                               std::span<u8> except_fds, i64 timeout_ms) {
    const auto is_set = [](std::span<u8> set, i32 fd) {
        return static_cast<usize>(fd / 8) < set.size() &&
               ((set[fd / 8] >> (fd % 8)) & 1) != 0;
    };
    const auto set_bit = [](std::span<u8> set, i32 fd) {
        set[fd / 8] |= static_cast<u8>(1 << (fd % 8));
    };

    const auto max_fd = static_cast<i32>(std::min<usize>(
        std::max(nfds, 0),
        std::max({read_fds.size(), write_fds.size(), except_fds.size()}) * 8));
    std::vector<i32> guest_fds;
    std::vector<pollfd> host_fds;
    for (i32 fd = 0; fd < max_fd; fd++) {
        short events = 0;
        if (is_set(read_fds, fd))
            events |= POLLIN;
        if (is_set(write_fds, fd))
            events |= POLLOUT;
        if (is_set(except_fds, fd))
            events |= POLLPRI;
        if (events == 0)
            continue;
        const auto* socket = get(fd);
        if (!socket)
            return {-1, E_BADF};
        guest_fds.push_back(fd);
        host_fds.push_back(pollfd{socket->host_fd, events, 0});
    }
    host_fds.push_back(pollfd{wake_pipe[0], POLLIN, 0});

    const int ret = ::poll(host_fds.data(), host_fds.size(),
                           toPollTimeout(timeout_ms));
    if (interrupted || host_fds.back().revents != 0)
        return {-1, E_INTR};
    if (ret < 0)
        return {-1, translateErrno(errno)};

    std::fill(read_fds.begin(), read_fds.end(), 0);
    std::fill(write_fds.begin(), write_fds.end(), 0);
    std::fill(except_fds.begin(), except_fds.end(), 0);
    i32 count = 0;
    for (usize i = 0; i < guest_fds.size(); i++) {
        const auto& host_fd = host_fds[i];
        if ((host_fd.events & POLLIN) &&
            (host_fd.revents & (POLLIN | POLLHUP | POLLERR))) {
            set_bit(read_fds, guest_fds[i]);
            count++;
        }
        if ((host_fd.events & POLLOUT) &&
            (host_fd.revents & (POLLOUT | POLLHUP | POLLERR))) {
            set_bit(write_fds, guest_fds[i]);
            count++;
        }
        if ((host_fd.events & POLLPRI) && (host_fd.revents & POLLPRI)) {
            set_bit(except_fds, guest_fds[i]);
            count++;
        }
    }
    return {count, 0};
}

bool HostSockets::isServerHttp(i32 fd) {
    const auto* socket = get(fd);
    return socket && socket->http.has_value();
}

} // namespace hydra::horizon::services::socket

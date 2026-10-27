#include "core/horizon/services/ssl/sf/ssl_connection.hpp"

#include <algorithm>

#include "core/horizon/services/socket/host_sockets.hpp"

namespace hydra::horizon::services::ssl::sf {

DEFINE_SERVICE_COMMAND_TABLE(
    ISslConnection, 0, setSocketDescriptor, 1, setHostName, 2, setVerifyOption,
    3, setIoMode, 4, getSocketDescriptor, 5, getHostName, 6, getVerifyOption, 7,
    getIoMode, 8, doHandshake, 9, doHandshakeGetServerCert, 10, read, 11, write,
    12, pending, 13, peek, 14, poll, 15, getVerifyCertError, 16,
    getNeededServerCertBufferSize, 17, setSessionCacheMode, 18,
    getSessionCacheMode, 19, flushSessionCache, 20, setRenegotiationMode, 21,
    getRenegotiationMode, 22, setOption, 23, getOption)

namespace {

// As yuzu's
constexpr result_t RESULT_NO_SOCKET = MAKE_RESULT(Ssl, 103);
constexpr result_t RESULT_INVALID_SOCKET = MAKE_RESULT(Ssl, 106);
constexpr result_t RESULT_WOULD_BLOCK = MAKE_RESULT(Ssl, 204);
constexpr result_t RESULT_TIMEOUT = MAKE_RESULT(Ssl, 205);
constexpr result_t RESULT_INTERNAL_ERROR = MAKE_RESULT(Ssl, 999);

constexpr u32 OPTION_DO_NOT_CLOSE_SOCKET = 1;
constexpr u32 OPTION_GET_SERVER_CERT_CHAIN = 2;

// nn::socket values
constexpr i32 MSG_PEEK_ = 0x2;
constexpr i32 MSG_DONTWAIT_ = 0x80;
constexpr u32 EAGAIN_ = 11;
constexpr u32 FIONREAD_ = 0x4004667f;
constexpr i16 POLLIN_ = 0x1;
constexpr i16 POLLPRI_ = 0x2;
constexpr i16 POLLOUT_ = 0x4;

// nn::ssl::Connection::PollEvent
constexpr u32 POLL_EVENT_READ = 1 << 0;
constexpr u32 POLL_EVENT_WRITE = 1 << 1;
constexpr u32 POLL_EVENT_EXCEPT = 1 << 2;

template <typename Buffer>
std::span<u8> getSpan(Buffer& buffer) {
    if (!buffer.stream)
        return {};
    return {buffer.stream->getPtr(),
            static_cast<usize>(buffer.stream->getSize())};
}

result_t toResult(const socket::CallResult& result) {
    if (result.ret >= 0)
        return RESULT_SUCCESS;
    return result.error == EAGAIN_ ? RESULT_WOULD_BLOCK : RESULT_INTERNAL_ERROR;
}

} // namespace

ISslConnection::~ISslConnection() {
    // The connection owns its socket unless the title keeps it
    if (fd >= 0 && !do_not_close_socket)
        sockets.close(fd);
}

result_t ISslConnection::passThroughHandshake() {
    if (fd < 0)
        return RESULT_NO_SOCKET;
    if (!sockets.isServerHttp(fd)) {
        LOG_WARN(Services,
                 "Online: TLS to {} is not supported (only the online "
                 "server's hosts pass through)",
                 host_name);
        return RESULT_INTERNAL_ERROR;
    }

    LOG_INFO(Services,
             "Online: TLS to {} passes through to the server's HTTP port",
             host_name);
    handshake_done = true;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setSocketDescriptor(i32 in_fd, i32* out_fd) {
    if (!sockets.exists(in_fd)) {
        *out_fd = -1;
        return RESULT_INVALID_SOCKET;
    }

    fd = in_fd;
    // As yuzu: the title gets the socket back only when it keeps it
    *out_fd = do_not_close_socket ? in_fd : -1;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setHostName(InBuffer<BufferAttr::MapAlias> buffer) {
    const auto span = getSpan(buffer);
    host_name.assign(span.begin(), std::find(span.begin(), span.end(), u8{0}));
    return RESULT_SUCCESS;
}

result_t ISslConnection::setVerifyOption(u32 option) {
    verify_option = option;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setIoMode(IoMode mode) {
    io_mode = mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::getSocketDescriptor(i32* out_fd) {
    *out_fd = fd;
    return RESULT_SUCCESS;
}

result_t
ISslConnection::getHostName(u32* out_size,
                            OutBuffer<BufferAttr::MapAlias> out_buffer) {
    const auto span = getSpan(out_buffer);
    const auto size = std::min(host_name.size(), span.size());
    std::copy_n(host_name.begin(), size, span.begin());
    *out_size = static_cast<u32>(size);
    return RESULT_SUCCESS;
}

result_t ISslConnection::getVerifyOption(u32* out_option) {
    *out_option = verify_option;
    return RESULT_SUCCESS;
}

result_t ISslConnection::getIoMode(IoMode* out_mode) {
    *out_mode = io_mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::doHandshake() { return passThroughHandshake(); }

result_t ISslConnection::doHandshakeGetServerCert(
    u32* out_size, u32* out_count, OutBuffer<BufferAttr::MapAlias> out_buffer) {
    (void)out_buffer;
    *out_size = 0;
    *out_count = 0;
    return passThroughHandshake();
}

result_t ISslConnection::read(u32* out_size,
                              OutBuffer<BufferAttr::MapAlias> out_buffer) {
    *out_size = 0;
    if (!handshake_done)
        return RESULT_INTERNAL_ERROR;

    // The I/O mode decides, not the socket's own flag
    const bool blocking = io_mode == IoMode::Blocking;
    const auto result =
        sockets.recv(fd, getSpan(out_buffer), blocking ? 0 : MSG_DONTWAIT_,
                     {}, nullptr, blocking);
    if (result.ret > 0)
        *out_size = static_cast<u32>(result.ret);
    return toResult(result);
}

result_t ISslConnection::write(InBuffer<BufferAttr::MapAlias> buffer,
                               u32* out_size) {
    *out_size = 0;
    if (!handshake_done)
        return RESULT_INTERNAL_ERROR;

    const bool blocking = io_mode == IoMode::Blocking;
    const auto result = sockets.send(fd, getSpan(buffer),
                                     blocking ? 0 : MSG_DONTWAIT_, {}, blocking);
    if (result.ret > 0)
        *out_size = static_cast<u32>(result.ret);
    return toResult(result);
}

result_t ISslConnection::pending(i32* out_size) {
    i32 count = 0;
    const auto result = sockets.ioctl(
        fd, FIONREAD_, {},
        std::span(reinterpret_cast<u8*>(&count), sizeof(count)));
    *out_size = result.ret < 0 ? 0 : count;
    return toResult(result);
}

result_t ISslConnection::peek(u32* out_size,
                              OutBuffer<BufferAttr::MapAlias> out_buffer) {
    *out_size = 0;
    if (!handshake_done)
        return RESULT_INTERNAL_ERROR;

    const auto result = sockets.recv(fd, getSpan(out_buffer),
                                     MSG_PEEK_ | MSG_DONTWAIT_, {}, nullptr);
    if (result.ret > 0)
        *out_size = static_cast<u32>(result.ret);
    return toResult(result);
}

result_t ISslConnection::poll(u32 events, u32 timeout_ms, u32* out_events) {
    *out_events = 0;
    struct {
        i32 fd;
        i16 events;
        i16 revents;
    } poll_fd{fd, 0, 0};
    if (events & POLL_EVENT_READ)
        poll_fd.events |= POLLIN_;
    if (events & POLL_EVENT_WRITE)
        poll_fd.events |= POLLOUT_;
    if (events & POLL_EVENT_EXCEPT)
        poll_fd.events |= POLLPRI_;

    const auto result = sockets.poll(
        std::span(reinterpret_cast<u8*>(&poll_fd), sizeof(poll_fd)),
        static_cast<i32>(timeout_ms));
    if (result.ret < 0)
        return toResult(result);
    if (result.ret == 0)
        return RESULT_TIMEOUT;

    if (poll_fd.revents & POLLIN_)
        *out_events |= POLL_EVENT_READ;
    if (poll_fd.revents & POLLOUT_)
        *out_events |= POLL_EVENT_WRITE;
    if (poll_fd.revents & POLLPRI_)
        *out_events |= POLL_EVENT_EXCEPT;
    return RESULT_SUCCESS;
}

result_t ISslConnection::getNeededServerCertBufferSize(u32* out_size) {
    *out_size = 0;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setSessionCacheMode(u32 mode) {
    session_cache_mode = mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::getSessionCacheMode(u32* out_mode) {
    *out_mode = session_cache_mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setRenegotiationMode(u32 mode) {
    renegotiation_mode = mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::getRenegotiationMode(u32* out_mode) {
    *out_mode = renegotiation_mode;
    return RESULT_SUCCESS;
}

result_t ISslConnection::setOption(u32 word0, u32 word1) {
    // A bool value and an option type (1 and up). libnx and yuzu disagree on
    // their order, which does not matter: the type is the larger word.
    const u32 option = std::max(word0, word1);
    const bool value = std::min(word0, word1) != 0;
    switch (option) {
    case OPTION_DO_NOT_CLOSE_SOCKET:
        do_not_close_socket = value;
        break;
    case OPTION_GET_SERVER_CERT_CHAIN:
        get_server_cert_chain = value;
        break;
    default:
        LOG_DEBUG(Services, "Option {} set to {}", option, value);
        break;
    }
    return RESULT_SUCCESS;
}

result_t ISslConnection::getOption(u32 option, bool* out_value) {
    switch (option) {
    case OPTION_DO_NOT_CLOSE_SOCKET:
        *out_value = do_not_close_socket;
        break;
    case OPTION_GET_SERVER_CERT_CHAIN:
        *out_value = get_server_cert_chain;
        break;
    default:
        *out_value = false;
        break;
    }
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::ssl::sf

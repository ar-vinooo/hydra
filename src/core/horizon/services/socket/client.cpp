#include "core/horizon/services/socket/client.hpp"

namespace hydra::horizon::services::socket {

DEFINE_SERVICE_COMMAND_TABLE(IClient, 0, registerClient, 1, startMonitoring, 2,
                             socket, 5, select, 6, poll, 8, recv, 9, recvFrom,
                             10, send, 11, sendTo, 12, accept, 13, bind, 14,
                             connect, 15, getPeerName, 16, getSockName, 17,
                             getSockOpt, 18, listen, 19, ioctl, 20, fcntl, 21,
                             setSockOpt, 22, shutdown, 24, write, 25, read, 26,
                             close)

namespace {
// nn::socket uses Linux errno values
constexpr u32 EAGAIN_ = 11;
constexpr u32 ENOTCONN_ = 107;
} // namespace

result_t IClient::registerClient(u64* out_unknown) {
    LOG_FUNC_STUBBED(Services);

    // HACK
    *out_unknown = 0;
    return RESULT_SUCCESS;
}

result_t IClient::select(i32* out_ret, u32* out_errno) {
    // Nothing ever becomes ready
    *out_ret = 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::recv(i32 fd, i32 flags, i32* out_ret, u32* out_errno,
                       OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    (void)fd;
    (void)flags;
    (void)out_buffer;
    *out_ret = -1;
    *out_errno = EAGAIN_;
    return RESULT_SUCCESS;
}

result_t IClient::recvFrom(i32 fd, i32 flags, i32* out_ret, u32* out_errno,
                           u32* out_addr_size,
                           OutBuffer<BufferAttr::AutoSelect> out_buffer,
                           OutBuffer<BufferAttr::AutoSelect> out_addr_buffer) {
    (void)fd;
    (void)flags;
    (void)out_buffer;
    (void)out_addr_buffer;
    *out_ret = -1;
    *out_errno = EAGAIN_;
    *out_addr_size = 0;
    return RESULT_SUCCESS;
}

result_t IClient::send(i32 fd, i32 flags,
                       InBuffer<BufferAttr::AutoSelect> buffer, i32* out_ret,
                       u32* out_errno) {
    (void)fd;
    (void)flags;
    *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize()) : 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::sendTo(i32 fd, i32 flags,
                         InBuffer<BufferAttr::AutoSelect> buffer,
                         InBuffer<BufferAttr::AutoSelect> addr_buffer,
                         i32* out_ret, u32* out_errno) {
    (void)fd;
    (void)flags;
    (void)addr_buffer;
    *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize()) : 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::accept(i32* out_ret, u32* out_errno) {
    *out_ret = -1;
    *out_errno = EAGAIN_;
    return RESULT_SUCCESS;
}

result_t IClient::getPeerName(i32* out_ret, u32* out_errno) {
    *out_ret = -1;
    *out_errno = ENOTCONN_;
    return RESULT_SUCCESS;
}

result_t IClient::getSockName(i32* out_ret, u32* out_errno) {
    *out_ret = 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::getSockOpt(i32* out_ret, u32* out_errno) {
    *out_ret = 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::ioctl(i32* out_ret, u32* out_errno) {
    *out_ret = 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::write(i32 fd, InBuffer<BufferAttr::AutoSelect> buffer,
                        i32* out_ret, u32* out_errno) {
    (void)fd;
    *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize()) : 0;
    *out_errno = 0;
    return RESULT_SUCCESS;
}

result_t IClient::read(i32* out_ret, u32* out_errno) {
    *out_ret = -1;
    *out_errno = EAGAIN_;
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::socket

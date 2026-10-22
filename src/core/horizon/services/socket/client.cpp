#include "core/horizon/services/socket/client.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/horizon/online/online.hpp"
#include "core/horizon/os.hpp"
#include "core/system.hpp"

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

template <typename Buffer>
std::span<u8> getSpan(Buffer& buffer) {
    if (!buffer.stream)
        return {};
    return {buffer.stream->getPtr(),
            static_cast<usize>(buffer.stream->getSize())};
}

void setResult(const CallResult& result, i32* out_ret, u32* out_errno) {
    *out_ret = result.ret;
    *out_errno = result.ret < 0 ? result.error : 0;
}
} // namespace

HostSockets* IClient::getHostSockets(System* system,
                                     kernel::Process* process) {
    if (!online::getConfig(process->getTitleId()))
        return nullptr;
    return &system->getOs().getHostSockets();
}

result_t IClient::registerClient(u64* out_unknown) {
    LOG_FUNC_STUBBED(Services);

    // HACK
    *out_unknown = 0;
    return RESULT_SUCCESS;
}

result_t IClient::socket(System* system, kernel::Process* process, u32 domain,
                         u32 type, u32 protocol, i32* out_ret,
                         u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->open(*online::getConfig(process->getTitleId()), domain,
                            type, protocol),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::select(System* system, kernel::Process* process,
                         SelectArgs args, i32* out_ret, u32* out_errno,
                         InBuffer<BufferAttr::AutoSelect> in_read_fds,
                         InBuffer<BufferAttr::AutoSelect> in_write_fds,
                         InBuffer<BufferAttr::AutoSelect> in_except_fds,
                         OutBuffer<BufferAttr::AutoSelect> out_read_fds,
                         OutBuffer<BufferAttr::AutoSelect> out_write_fds,
                         OutBuffer<BufferAttr::AutoSelect> out_except_fds) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        // Nothing ever becomes ready
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    const auto copy_in = [](auto& buffer) {
        const auto span = getSpan(buffer);
        return std::vector<u8>(span.begin(), span.end());
    };
    auto read_fds = copy_in(in_read_fds);
    auto write_fds = copy_in(in_write_fds);
    auto except_fds = copy_in(in_except_fds);

    const i64 timeout_ms =
        args.null_timeout
            ? -1
            : args.timeout_sec * 1000 + (args.timeout_usec + 999) / 1000;
    setResult(sockets->select(args.nfds, read_fds, write_fds, except_fds,
                              timeout_ms),
              out_ret, out_errno);

    const auto copy_out = [](const std::vector<u8>& fds, auto& buffer) {
        const auto span = getSpan(buffer);
        std::copy_n(fds.begin(), std::min(fds.size(), span.size()),
                    span.begin());
    };
    copy_out(read_fds, out_read_fds);
    copy_out(write_fds, out_write_fds);
    copy_out(except_fds, out_except_fds);
    return RESULT_SUCCESS;
}

result_t IClient::poll(System* system, kernel::Process* process, u32 nfds,
                       i32 timeout, i32* out_ret, u32* out_errno,
                       InBuffer<BufferAttr::AutoSelect> in_fds,
                       OutBuffer<BufferAttr::AutoSelect> out_fds) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    const auto in_span = getSpan(in_fds);
    const auto poll_span = in_span.first(
        std::min<usize>(in_span.size(), usize{nfds} * 8));
    std::vector<u8> fds(poll_span.begin(), poll_span.end());
    setResult(sockets->poll(fds, timeout), out_ret, out_errno);

    const auto out_span = getSpan(out_fds);
    std::copy_n(fds.begin(), std::min(fds.size(), out_span.size()),
                out_span.begin());
    return RESULT_SUCCESS;
}

result_t IClient::recv(System* system, kernel::Process* process, i32 fd,
                       i32 flags, i32* out_ret, u32* out_errno,
                       OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = -1;
        *out_errno = EAGAIN_;
        return RESULT_SUCCESS;
    }

    setResult(sockets->recv(fd, getSpan(out_buffer), flags, {}, nullptr),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::recvFrom(System* system, kernel::Process* process, i32 fd,
                           i32 flags, i32* out_ret, u32* out_errno,
                           u32* out_addr_size,
                           OutBuffer<BufferAttr::AutoSelect> out_buffer,
                           OutBuffer<BufferAttr::AutoSelect> out_addr_buffer) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = -1;
        *out_errno = EAGAIN_;
        *out_addr_size = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->recv(fd, getSpan(out_buffer), flags,
                            getSpan(out_addr_buffer), out_addr_size),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::send(System* system, kernel::Process* process, i32 fd,
                       i32 flags, InBuffer<BufferAttr::AutoSelect> buffer,
                       i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize())
                                 : 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->send(fd, getSpan(buffer), flags, {}), out_ret,
              out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::sendTo(System* system, kernel::Process* process, i32 fd,
                         i32 flags, InBuffer<BufferAttr::AutoSelect> buffer,
                         InBuffer<BufferAttr::AutoSelect> addr_buffer,
                         i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize())
                                 : 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->send(fd, getSpan(buffer), flags, getSpan(addr_buffer)),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::accept(System* system, kernel::Process* process, i32 fd,
                         i32* out_ret, u32* out_errno, u32* out_addr_size,
                         OutBuffer<BufferAttr::AutoSelect> out_addr_buffer) {
    *out_addr_size = 0;
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = -1;
        *out_errno = EAGAIN_;
        return RESULT_SUCCESS;
    }

    setResult(sockets->accept(fd, getSpan(out_addr_buffer), *out_addr_size),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::bind(System* system, kernel::Process* process, i32 fd,
                       InBuffer<BufferAttr::AutoSelect> addr_buffer,
                       i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->bind(fd, getSpan(addr_buffer)), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::connect(System* system, kernel::Process* process, i32 fd,
                          InBuffer<BufferAttr::AutoSelect> addr_buffer,
                          i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->connect(fd, getSpan(addr_buffer)), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t
IClient::getPeerName(System* system, kernel::Process* process, i32 fd,
                     i32* out_ret, u32* out_errno, u32* out_addr_size,
                     OutBuffer<BufferAttr::AutoSelect> out_addr_buffer) {
    *out_addr_size = 0;
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = -1;
        *out_errno = ENOTCONN_;
        return RESULT_SUCCESS;
    }

    setResult(
        sockets->getPeerName(fd, getSpan(out_addr_buffer), *out_addr_size),
        out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t
IClient::getSockName(System* system, kernel::Process* process, i32 fd,
                     i32* out_ret, u32* out_errno, u32* out_addr_size,
                     OutBuffer<BufferAttr::AutoSelect> out_addr_buffer) {
    *out_addr_size = 0;
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(
        sockets->getSockName(fd, getSpan(out_addr_buffer), *out_addr_size),
        out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::getSockOpt(System* system, kernel::Process* process, i32 fd,
                             i32 level, i32 name, i32* out_ret,
                             u32* out_errno, u32* out_size,
                             OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    *out_size = 0;
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(
        sockets->getSockOpt(fd, level, name, getSpan(out_buffer), *out_size),
        out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::listen(System* system, kernel::Process* process, i32 fd,
                         i32 backlog, i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->listen(fd, backlog), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::ioctl(System* system, kernel::Process* process, i32 fd,
                        u32 request, u32 buffer_count, i32* out_ret,
                        u32* out_errno,
                        InBuffer<BufferAttr::AutoSelect> in_buffer,
                        OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    (void)buffer_count;
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->ioctl(fd, request, getSpan(in_buffer),
                             getSpan(out_buffer)),
              out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::fcntl(System* system, kernel::Process* process, i32 fd,
                        i32 cmd, i32 arg, i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->fcntl(fd, cmd, arg), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::setSockOpt(System* system, kernel::Process* process, i32 fd,
                             i32 level, i32 name,
                             InBuffer<BufferAttr::AutoSelect> buffer,
                             i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->setSockOpt(fd, level, name, getSpan(buffer)), out_ret,
              out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::shutdown(System* system, kernel::Process* process, i32 fd,
                           i32 how, i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->shutdown(fd, how), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::write(System* system, kernel::Process* process, i32 fd,
                        InBuffer<BufferAttr::AutoSelect> buffer, i32* out_ret,
                        u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = buffer.stream ? static_cast<i32>(buffer.stream->getSize())
                                 : 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->send(fd, getSpan(buffer), 0, {}), out_ret, out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::read(System* system, kernel::Process* process, i32 fd,
                       i32* out_ret, u32* out_errno,
                       OutBuffer<BufferAttr::AutoSelect> out_buffer) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = -1;
        *out_errno = EAGAIN_;
        return RESULT_SUCCESS;
    }

    setResult(sockets->recv(fd, getSpan(out_buffer), 0, {}, nullptr), out_ret,
              out_errno);
    return RESULT_SUCCESS;
}

result_t IClient::close(System* system, kernel::Process* process, i32 fd,
                        i32* out_ret, u32* out_errno) {
    auto* sockets = getHostSockets(system, process);
    if (!sockets) {
        *out_ret = 0;
        *out_errno = 0;
        return RESULT_SUCCESS;
    }

    setResult(sockets->close(fd), out_ret, out_errno);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::socket

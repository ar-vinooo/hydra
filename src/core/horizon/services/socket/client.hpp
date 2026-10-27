#pragma once

#include "core/horizon/services/const.hpp"
#include "core/horizon/services/socket/host_sockets.hpp"

namespace hydra::horizon::services::socket {

class IClient : public IService {
  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    struct SelectArgs {
        i32 nfds;
        u32 pad;
        i64 timeout_sec;
        i64 timeout_usec;
        bool null_timeout;
    };

    // The host sockets when the title plays online (online/<title ID>.toml),
    // nullptr when there is no network: then sends succeed without going
    // anywhere and receives never have data. Errors are reported through
    // errno, as unknown commands make the SDK abort.
    static HostSockets* getHostSockets(System* system,
                                       kernel::Process* process);

    // Commands
    result_t registerClient(u64* out_unknown);
    STUB_REQUEST_COMMAND(startMonitoring);
    result_t socket(System* system, kernel::Process* process, u32 domain,
                    u32 type, u32 protocol, i32* out_ret, u32* out_errno);
    result_t select(System* system, kernel::Process* process,
                    SelectArgs args, i32* out_ret, u32* out_errno,
                    InBuffer<BufferAttr::AutoSelect> in_read_fds,
                    InBuffer<BufferAttr::AutoSelect> in_write_fds,
                    InBuffer<BufferAttr::AutoSelect> in_except_fds,
                    OutBuffer<BufferAttr::AutoSelect> out_read_fds,
                    OutBuffer<BufferAttr::AutoSelect> out_write_fds,
                    OutBuffer<BufferAttr::AutoSelect> out_except_fds);
    result_t poll(System* system, kernel::Process* process, u32 nfds,
                  i32 timeout, i32* out_ret, u32* out_errno,
                  InBuffer<BufferAttr::AutoSelect> in_fds,
                  OutBuffer<BufferAttr::AutoSelect> out_fds);
    result_t recv(System* system, kernel::Process* process, i32 fd, i32 flags,
                  i32* out_ret, u32* out_errno,
                  OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t recvFrom(System* system, kernel::Process* process, i32 fd,
                      i32 flags, i32* out_ret, u32* out_errno,
                      u32* out_addr_size,
                      OutBuffer<BufferAttr::AutoSelect> out_buffer,
                      OutBuffer<BufferAttr::AutoSelect> out_addr_buffer);
    result_t send(System* system, kernel::Process* process, i32 fd, i32 flags,
                  InBuffer<BufferAttr::AutoSelect> buffer, i32* out_ret,
                  u32* out_errno);
    result_t sendTo(System* system, kernel::Process* process, i32 fd,
                    i32 flags, InBuffer<BufferAttr::AutoSelect> buffer,
                    InBuffer<BufferAttr::AutoSelect> addr_buffer,
                    i32* out_ret, u32* out_errno);
    result_t accept(System* system, kernel::Process* process, i32 fd,
                    i32* out_ret, u32* out_errno, u32* out_addr_size,
                    OutBuffer<BufferAttr::AutoSelect> out_addr_buffer);
    result_t bind(System* system, kernel::Process* process, i32 fd,
                  InBuffer<BufferAttr::AutoSelect> addr_buffer, i32* out_ret,
                  u32* out_errno);
    result_t connect(System* system, kernel::Process* process, i32 fd,
                     InBuffer<BufferAttr::AutoSelect> addr_buffer,
                     i32* out_ret, u32* out_errno);
    result_t getPeerName(System* system, kernel::Process* process, i32 fd,
                         i32* out_ret, u32* out_errno, u32* out_addr_size,
                         OutBuffer<BufferAttr::AutoSelect> out_addr_buffer);
    result_t getSockName(System* system, kernel::Process* process, i32 fd,
                         i32* out_ret, u32* out_errno, u32* out_addr_size,
                         OutBuffer<BufferAttr::AutoSelect> out_addr_buffer);
    result_t getSockOpt(System* system, kernel::Process* process, i32 fd,
                        i32 level, i32 name, i32* out_ret, u32* out_errno,
                        u32* out_size,
                        OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t listen(System* system, kernel::Process* process, i32 fd,
                    i32 backlog, i32* out_ret, u32* out_errno);
    result_t ioctl(System* system, kernel::Process* process, i32 fd,
                   u32 request, u32 buffer_count, i32* out_ret,
                   u32* out_errno, InBuffer<BufferAttr::AutoSelect> in_buffer,
                   OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t fcntl(System* system, kernel::Process* process, i32 fd, i32 cmd,
                   i32 arg, i32* out_ret, u32* out_errno);
    result_t setSockOpt(System* system, kernel::Process* process, i32 fd,
                        i32 level, i32 name,
                        InBuffer<BufferAttr::AutoSelect> buffer, i32* out_ret,
                        u32* out_errno);
    result_t shutdown(System* system, kernel::Process* process, i32 fd,
                      i32 how, i32* out_ret, u32* out_errno);
    result_t write(System* system, kernel::Process* process, i32 fd,
                   InBuffer<BufferAttr::AutoSelect> buffer, i32* out_ret,
                   u32* out_errno);
    result_t read(System* system, kernel::Process* process, i32 fd,
                  i32* out_ret, u32* out_errno,
                  OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t close(System* system, kernel::Process* process, i32 fd,
                   i32* out_ret, u32* out_errno);
};

} // namespace hydra::horizon::services::socket

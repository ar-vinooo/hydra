#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::socket {

class IClient : public IService {
  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // Commands
    result_t registerClient(u64* out_unknown);
    STUB_REQUEST_COMMAND(startMonitoring);
    STUB_REQUEST_COMMAND(socket);
    STUB_REQUEST_COMMAND(poll);
    STUB_REQUEST_COMMAND(bind);
    STUB_REQUEST_COMMAND(connect);
    STUB_REQUEST_COMMAND(listen);
    STUB_REQUEST_COMMAND(fcntl);
    STUB_REQUEST_COMMAND(setSockOpt);
    STUB_REQUEST_COMMAND(shutdown);
    STUB_REQUEST_COMMAND(close);

    // There is no network: sends succeed without going anywhere and receives
    // never have data. Errors are reported through errno, as unknown commands
    // make the SDK abort.
    result_t select(i32* out_ret, u32* out_errno);
    result_t recv(i32 fd, i32 flags, i32* out_ret, u32* out_errno,
                  OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t recvFrom(i32 fd, i32 flags, i32* out_ret, u32* out_errno,
                      u32* out_addr_size,
                      OutBuffer<BufferAttr::AutoSelect> out_buffer,
                      OutBuffer<BufferAttr::AutoSelect> out_addr_buffer);
    result_t send(i32 fd, i32 flags, InBuffer<BufferAttr::AutoSelect> buffer,
                  i32* out_ret, u32* out_errno);
    result_t sendTo(i32 fd, i32 flags, InBuffer<BufferAttr::AutoSelect> buffer,
                    InBuffer<BufferAttr::AutoSelect> addr_buffer, i32* out_ret,
                    u32* out_errno);
    result_t accept(i32* out_ret, u32* out_errno);
    result_t getPeerName(i32* out_ret, u32* out_errno);
    result_t getSockName(i32* out_ret, u32* out_errno);
    result_t getSockOpt(i32* out_ret, u32* out_errno);
    result_t ioctl(i32* out_ret, u32* out_errno);
    result_t write(i32 fd, InBuffer<BufferAttr::AutoSelect> buffer,
                   i32* out_ret, u32* out_errno);
    result_t read(i32* out_ret, u32* out_errno);
};

} // namespace hydra::horizon::services::socket

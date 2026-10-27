#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::socket::resolver {

class IResolver : public IService {
  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // Commands
    result_t getHostByNameRequest(kernel::Process* process,
                                  InBuffer<BufferAttr::MapAlias> name_buffer,
                                  u32* out_host_error, u32* out_errno,
                                  u32* out_size,
                                  OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t getAddrInfoRequest(kernel::Process* process,
                                InBuffer<BufferAttr::MapAlias> node_buffer,
                                InBuffer<BufferAttr::MapAlias> service_buffer,
                                InBuffer<BufferAttr::MapAlias> hints_buffer,
                                u32* out_errno, i32* out_ret, u32* out_size,
                                OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t getHostByNameRequestWithOptions(
        kernel::Process* process,
        InBuffer<BufferAttr::AutoSelect> name_buffer,
        InBuffer<BufferAttr::AutoSelect> options_buffer, u32* out_size,
        i32* out_host_error, i32* out_errno,
        OutBuffer<BufferAttr::AutoSelect> out_buffer);
    result_t getAddrInfoRequestWithOptions(
        kernel::Process* process,
        InBuffer<BufferAttr::AutoSelect> node_buffer,
        InBuffer<BufferAttr::AutoSelect> service_buffer,
        InBuffer<BufferAttr::AutoSelect> hints_buffer,
        InBuffer<BufferAttr::AutoSelect> options_buffer, u32* out_size,
        i32* out_ret, i32* out_host_error, i32* out_errno,
        OutBuffer<BufferAttr::AutoSelect> out_buffer);
};

} // namespace hydra::horizon::services::socket::resolver

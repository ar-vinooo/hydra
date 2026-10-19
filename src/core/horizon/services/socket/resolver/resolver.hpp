#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::socket::resolver {

class IResolver : public IService {
  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // Commands
    result_t getAddrInfoRequest(u32* out_errno, i32* out_ret, u32* out_size);
    result_t getAddrInfoRequestWithOptions(u32* out_size, i32* out_ret,
                                         i32* out_host_error, i32* out_errno);
};

} // namespace hydra::horizon::services::socket::resolver

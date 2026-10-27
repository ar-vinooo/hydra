#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::account::baas {

class IManagerForApplication : public IService {
  public:
    explicit IManagerForApplication(uuid_t user_id_) : user_id{user_id_} {}

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // TODO: use
    [[maybe_unused]] uuid_t user_id;

    // Commands
    STUB_REQUEST_COMMAND(checkAvailability);
    result_t getAccountId(kernel::Process* process, u64* out_id);
    result_t ensureIdTokenCacheAsync(RequestContext* ctx);
    result_t loadIdTokenCache(kernel::Process* process, u32* out_size,
                              OutBuffer<BufferAttr::MapAlias> out_buffer);
};

} // namespace hydra::horizon::services::account::baas

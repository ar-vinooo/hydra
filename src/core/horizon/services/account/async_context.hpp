#pragma once

#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::account {

// An asynchronous operation that is already done when the title gets it
class IAsyncContext : public IService {
  public:
    explicit IAsyncContext(result_t result_ = RESULT_SUCCESS);

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    result_t result;
    kernel::AutoObjectPtr<kernel::Event> event;

    // Commands
    result_t getSystemEvent(kernel::Process* process,
                            OutHandle<HandleAttr::Copy> out_handle);
    STUB_REQUEST_COMMAND(cancel);
    result_t hasDone(bool* out_done);
    result_t getResult();
};

} // namespace hydra::horizon::services::account

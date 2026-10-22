#include "core/horizon/services/account/async_context.hpp"

#include "core/horizon/kernel/process.hpp"

namespace hydra::horizon::services::account {

DEFINE_SERVICE_COMMAND_TABLE(IAsyncContext, 0, getSystemEvent, 1, cancel, 2,
                             hasDone, 3, getResult)

IAsyncContext::IAsyncContext(result_t result_)
    : result{result_}, event{std::make_unique<kernel::Event>(
                           true, "IAsyncContext system event")} {}

result_t
IAsyncContext::getSystemEvent(kernel::Process* process,
                              OutHandle<HandleAttr::Copy> out_handle) {
    out_handle = process->addHandle(event.get());
    return RESULT_SUCCESS;
}

result_t IAsyncContext::hasDone(bool* out_done) {
    *out_done = true;
    return RESULT_SUCCESS;
}

result_t IAsyncContext::getResult() { return result; }

} // namespace hydra::horizon::services::account

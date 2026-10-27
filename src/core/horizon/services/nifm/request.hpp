#pragma once

#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::nifm {

enum class RequestState {
    Invalid = 0,
    Free = 1,
    OnHold = 2,
    Accepted = 3,
    Blocking = 4,
};

enum class ConnectionConfirmationOption : u32 {
    Invalid = 0,
    Prohibited = 1,
    NotRequired = 2,
    Preferred = 3,
    Required = 4,
    Forced = 5,
};

class IRequest : public IService {
  public:
    // online: the title plays online (online/<title ID>.toml), and its
    // requests are accepted
    explicit IRequest(bool online_ = false);

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    bool online;
    std::array<kernel::AutoObjectPtr<kernel::Event>, 2> events;

    // Commands
    result_t getRequestState(RequestState* out_state);
    STUB_REQUEST_COMMAND(getResult);
    result_t
    getSystemEventReadableHandles(kernel::Process* process,
                                  OutHandle<HandleAttr::Copy> out_handle0,
                                  OutHandle<HandleAttr::Copy> out_handle1);
    STUB_REQUEST_COMMAND(cancel);
    result_t submit();
    STUB_REQUEST_COMMAND(setRequirementPreset);
    result_t
    setConnectionConfirmationOption(ConnectionConfirmationOption option);
};

} // namespace hydra::horizon::services::nifm

ENABLE_ENUM_FORMATTING(
    hydra::horizon::services::nifm::ConnectionConfirmationOption, Invalid,
    "invalid", Prohibited, "prohibited", NotRequired, "not_required", Preferred,
    "preferred", Required, "required", Forced, "forced")

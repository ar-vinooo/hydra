#include "core/horizon/applets/error/applet.hpp"

#include "core/horizon/applets/error/const.hpp"
#include "core/horizon/ui/handler_base.hpp"
#include "core/system.hpp"

namespace hydra::horizon::applets::error {

result_t Applet::run(System& system) {
    // TODO: PopInData
    auto param_reader = popInDataRaw();
    const auto param_common = param_reader.read<ParamCommon>();
    LOG_INFO(Applets, "Error applet: type {}", param_common.type);

    // TODO: context (if present)

    ReturnValue ret;
    // NOLINTNEXTLINE(readability-trivial-switch)
    switch (param_common.type) {
    case ErrorType::ApplicationError: {
        const auto param = param_reader.read<ParamForApplicationError>();
        LOG_INFO(Applets, "Error applet: code 0x{:x}, message \"{}\"",
                 param.error_code_number,
                 std::string_view(param.dialog_message,
                                  strnlen(param.dialog_message,
                                          sizeof(param.dialog_message))));

        // TODO: handle empty messages
        system.getUiHandler().showMessageDialog(
            ui::MessageDialogType::Error,
            fmt::format("Error (0x{:x})", param.error_code_number),
            fmt::format("{}\n{}", param.dialog_message,
                        param.fullscreen_message));
        // TODO: details

        if (param_common.is_jump_enabled) {
            // TODO: set
        }

        break;
    }
    default:
        LOG_NOT_IMPLEMENTED(Applets, "Error type {}", param_common.type);
        break;
    }

    pushOutData(ret);

    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::applets::error

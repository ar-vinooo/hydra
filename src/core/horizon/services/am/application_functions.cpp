#include "core/horizon/services/am/application_functions.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/horizon/services/am/storage.hpp"
#include "core/horizon/services/ns/const.hpp"

namespace hydra::horizon::services::am {

DEFINE_SERVICE_COMMAND_TABLE(
    IApplicationFunctions, 1, popLaunchParameter, 20, ensureSaveData, 21,
    getDesiredLanguage, 22, setTerminateResult, 23, getDisplayVersion, 25,
    extendSaveData, 26, getSaveDataSize, 32, beginBlockingHomeButton, 33,
    endBlockingHomeButton, 40, notifyRunning, 50, getPseudoDeviceId, 65,
    isGamePlayRecordingSupported, 66, initializeGamePlayRecording, 67,
    setGamePlayRecordingState, 90, enableApplicationCrashReport, 130,
    getGpuErrorDetectedSystemEvent)

result_t
IApplicationFunctions::popLaunchParameter(kernel::Process* process,
                                          RequestContext* ctx,
                                          kernel::LaunchParameterKind kind) {
    LOG_DEBUG(Services, "Kind: {}", kind);

    auto data = process->getAppletState().popLaunchParameter(kind);
    if (data.empty())
        return MAKE_RESULT(Am, 1); // TODO: result code

    addService(*ctx, new IStorage(std::move(data)));
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::ensureSaveData(uuid_t user_id,
                                               u64* out_required_size) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "user ID: 0x{:016x}", user_id);

    // TODO: check if there is enough space for the save data
    *out_required_size = 0x0;
    return RESULT_SUCCESS;
}

result_t
IApplicationFunctions::getDesiredLanguage(LanguageCode* out_language_code) {
    *out_language_code = toLanguageCode(CONFIG_INSTANCE.getSystemLanguage());
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::setTerminateResult(result_t result) {
    const auto module = GET_RESULT_MODULE(result);
    const auto description = GET_RESULT_DESCRIPTION(result);
    LOG_INFO(Kernel, "Module: {}, description: {}", module, description);

    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::getDisplayVersion(RequestContext* ctx,
                                                  DisplayVersion* out_version) {
    // From control.nacp; without one (homebrew), the first version
    *out_version = {.name = "1.0.0"};
    if (const auto nacp = ctx->process->getApplicationControlProperty()) {
        static_assert(sizeof(out_version->name) ==
                      sizeof(nacp->display_version));
        std::memcpy(out_version->name, nacp->display_version,
                    sizeof(out_version->name));
        out_version->name[sizeof(out_version->name) - 1] = '\0';
    }
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::extendSaveData(u128 user_id, i64 size,
                                               i64 journal_size,
                                               result_t* out_result) {
    LOG_FUNC_WITH_ARGS_STUBBED(
        Services, "user ID: 0x{:016x}, size: 0x{:08x}, Journal size: 0x{:08x}",
        user_id, size, journal_size);

    // HACK
    // TODO: why 2 results?
    *out_result = RESULT_SUCCESS;

    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::getSaveDataSize(RequestContext* ctx,
                                                i64* out_size,
                                                i64* out_journal_size) {
    // The sizes the application declares for its account save data. The
    // save data type and user (ignored) select between account and device
    // data, which games ask for their account data.
    *out_size = 0;
    *out_journal_size = 0;
    if (const auto nacp = ctx->process->getApplicationControlProperty()) {
        *out_size = static_cast<i64>(nacp->user_account_save_data_size);
        *out_journal_size =
            static_cast<i64>(nacp->user_account_save_data_journal_size);
    }
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::getPseudoDeviceId(u128* out_id) {
    LOG_FUNC_STUBBED(Services);

    // HACK
    *out_id = "dev_id"_u64;
    return RESULT_SUCCESS;
}

result_t
IApplicationFunctions::isGamePlayRecordingSupported(bool* out_enabled) {
    *out_enabled =
        (game_play_recording_state == GamePlayRecordingState::Enabled);
    return RESULT_SUCCESS;
}

result_t
IApplicationFunctions::setGamePlayRecordingState(GamePlayRecordingState state) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "state: {}", state);
    game_play_recording_state = state;
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::enableApplicationCrashReport(bool enabled) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "enabled: {}", enabled);
    return RESULT_SUCCESS;
}

result_t IApplicationFunctions::getGpuErrorDetectedSystemEvent(
    kernel::Process* process, OutHandle<HandleAttr::Copy> out_handle) {
    out_handle = process->addHandle(gpu_error_detect_event);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::am

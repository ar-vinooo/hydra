#include "core/horizon/services/mmnv/request.hpp"

namespace hydra::horizon::services::mmnv {

namespace {
u32 effectiveFrequency(Module module, u32 requested) {
    // A clock request is a floor, not a literal hardware rate. NVDEC clients
    // probe the maximum with INT_MAX kHz, which wraps to 0xfffffc18 Hz.
    // Bound the virtual engine to a valid Tegra X1 NVDEC rate (408 MHz).
    // Reference: hekate bdk/soc/clock.c, _clock_nvdec.
    return module == Module::Nvdec ? std::min(requested, 408000000u) : requested;
}
}

DEFINE_SERVICE_COMMAND_TABLE(IRequest, 0, initializeOld, 1, finalizeOld, 2,
                             setAndWaitOld, 3, getOld, 4, initialize, 5,
                             finalize, 6, setAndWait, 7, get)

result_t IRequest::initializeOld(Module module, u32 priority,
                                 u32 event_clear_mode) {
    std::lock_guard lock(request_mutex);
    legacy_frequencies[static_cast<u32>(module)] = 0;
    LOG_INFO(Services, "Initialize legacy clock request: {}, priority {}, clear {}",
              module, priority, event_clear_mode);
    return RESULT_SUCCESS;
}

result_t IRequest::finalizeOld(Module module) {
    std::lock_guard lock(request_mutex);
    legacy_frequencies.erase(static_cast<u32>(module));
    return RESULT_SUCCESS;
}

result_t IRequest::setAndWaitOld(Module module, u32 frequency_hz,
                                 [[maybe_unused]] i32 timeout) {
    std::lock_guard lock(request_mutex);
    const auto it = legacy_frequencies.find(static_cast<u32>(module));
    if (it == legacy_frequencies.end())
        return MAKE_RESULT(Svc, kernel::Error::NotFound);
    it->second = effectiveFrequency(module, frequency_hz);
    return RESULT_SUCCESS;
}

result_t IRequest::getOld(Module module, u32* out_setting_current) {
    std::lock_guard lock(request_mutex);
    const auto it = legacy_frequencies.find(static_cast<u32>(module));
    if (it == legacy_frequencies.end()) {
        LOG_WARN(Services, "Legacy clock get for uninitialized module {}", module);
        return MAKE_RESULT(Svc, kernel::Error::NotFound);
    }
    *out_setting_current = it->second;
    LOG_INFO(Services, "Legacy clock get {} returns {} Hz", module, it->second);
    return RESULT_SUCCESS;
}

result_t IRequest::initialize(Module module, u32 priority, u32 event_clear_mode,
                              u32* out_request_id) {
    std::lock_guard lock(request_mutex);
    *out_request_id = next_request_id++;
    request_frequencies.emplace(*out_request_id, 0);
    request_modules.emplace(*out_request_id, module);
    LOG_INFO(Services, "Initialize clock request {}: {}, priority {}, clear {}",
              *out_request_id, module, priority, event_clear_mode);
    return RESULT_SUCCESS;
}

result_t IRequest::finalize(u32 request_id) {
    std::lock_guard lock(request_mutex);
    request_frequencies.erase(request_id);
    request_modules.erase(request_id);
    return RESULT_SUCCESS;
}

result_t IRequest::setAndWait(u32 request_id, u32 frequency_hz,
                              i32 timeout) {
    std::lock_guard lock(request_mutex);
    const auto it = request_frequencies.find(request_id);
    if (it == request_frequencies.end())
        return MAKE_RESULT(Svc, kernel::Error::NotFound);
    it->second = effectiveFrequency(request_modules.at(request_id), frequency_hz);
    LOG_INFO(Services, "Clock request {} set to {} Hz (third argument {})", request_id, frequency_hz, timeout);
    return RESULT_SUCCESS;
}

result_t IRequest::get(u32 request_id, u32* out_setting_current) {
    std::lock_guard lock(request_mutex);
    const auto it = request_frequencies.find(request_id);
    if (it == request_frequencies.end()) {
        LOG_WARN(Services, "Clock get for unknown request {}", request_id);
        return MAKE_RESULT(Svc, kernel::Error::NotFound);
    }
    *out_setting_current = it->second;
    LOG_INFO(Services, "Clock get request {} returns {} Hz", request_id, it->second);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::mmnv

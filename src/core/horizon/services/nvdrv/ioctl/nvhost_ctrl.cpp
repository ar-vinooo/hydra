#include "core/horizon/services/nvdrv/ioctl/nvhost_ctrl.hpp"

#include "core/horizon/kernel/event.hpp"
#include "core/system.hpp"
#include "core/horizon/services/settings/nx_settings.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

NvHostCtrl::~NvHostCtrl() {
    for (auto& event : events) {
        if (syncpoints != nullptr)
            syncpoints->cancelWait(event.wait_token);
        if (event.event != nullptr)
            event.event->release();
    }
}

DEFINE_IOCTL_TABLE(NvHostCtrl,
                   DEFINE_IOCTL_TABLE_ENTRY(NvHostCtrl, 0x00, 0x14, syncptRead, 0x1b, getConfig,
                                            0x1d, syncptWaitEvent, 0x1e,
                                            syncptWaitEventEx, 0x1f,
                                            syncptAllocEvent, 0x20,
                                            syncptFreeEvent))

NvResult NvHostCtrl::queryEvent(u32 event_id_u32, kernel::Event*& out_event) {
    u32 slot;
    u32 syncpoint_id;
    if (extractBits(event_id_u32, 28, 1) != 0u) { // New format
        slot = extractBits(event_id_u32, 0, 16);
        syncpoint_id = extractBits(event_id_u32, 16, 12);
    } else { // Old format
        slot = extractBits(event_id_u32, 0, 8);
        syncpoint_id = extractBits(event_id_u32, 4, 28);
    }

    if (slot >= EVENT_COUNT) {
        LOG_WARN(Services, "Invalid event at slot {}", slot);
        return NvResult::NotSupported; // TODO: NvInternalResult::InvalidInput
    }

    const auto& event = events[slot];
    // TODO: check if syncpoint IDs are equal
    (void)syncpoint_id;

    out_event = event.event;
    return NvResult::Success;
}

NvResult NvHostCtrl::getConfig(std::array<char, 0x41> name,
                               std::array<char, 0x41> key,
                               std::array<u8, 0x101>* out_value) {
    const auto key_str = toLower(fmt::format("{}!{}", name.data(), key.data()));
    LOG_DEBUG(Services, "Key: {}", key_str);

    auto it = settings::nx_settings.find(key_str);
    if (it == settings::nx_settings.end()) {
        LOG_WARN(Services, "Key not found: {}", key_str);
        // TODO: return NvInternalResult::InvalidInput?
        return NvResult::NotAvailableInProduction;
    }

    const auto& value = it->second;
    auto ptr = out_value->data();
    switch (value.type) {
    case settings::SettingDataType::String:
        std::memcpy(ptr, value.s.data(), value.s.size());
        break;
    case settings::SettingDataType::Integer:
        *reinterpret_cast<i32*>(ptr) = value.i;
        break;
    case settings::SettingDataType::Boolean:
        *reinterpret_cast<bool*>(ptr) = value.b;
        break;
    }

    return NvResult::Success;
}

NvResult NvHostCtrl::syncptWaitEvent(IoctlContext* context, u32 id, u32 tresh, i32 timeout,
                                     u32* out_value) {
    ONCE(LOG_FUNC_WITH_ARGS_STUBBED(
        Services, "ID: {}, treshold: {}, timeout: {}", id, tresh, timeout));

    if (id == 0) { *out_value = 0; return NvResult::Success; } // Existing GPU path.
    auto& gpu = context->system.getGpu();
    auto& syncpoints_ref = gpu.getVideoSyncpoints();
    // A GPU fence is reached when the host finishes the work: wait for it,
    // bounded so a lost fence cannot hang the guest
    if (gpu.isFenceSyncpoint(id)) {
        const auto wait = timeout < 0 ? std::chrono::milliseconds(10000)
                                      : std::chrono::milliseconds(timeout);
        if (!syncpoints_ref.waitFor(id, tresh, wait))
            ONCE(LOG_WARN(Services, "GPU fence {}:{} not reached in {} ms", id,
                          tresh, wait.count()));
    }
    const auto value = syncpoints_ref.read(id);
    if (!value) return NvResult::BadValue;
    *out_value = *value;
    return video::Syncpoints::reached(*value, tresh) ? NvResult::Success : NvResult::Timeout;
}

NvResult NvHostCtrl::syncptWaitEventEx(IoctlContext* context, u32 id, u32 tresh, i32 timeout,
                                       InOutSingle<u32> out_value) {
    ONCE(LOG_FUNC_WITH_ARGS_STUBBED(
        Services, "ID: {}, treshold: {}, timeout: {}", id, tresh, timeout));

    if (id == 0) { out_value = 0; return NvResult::Success; } // Existing GPU path.
    syncpoints = &context->system.getGpu().getVideoSyncpoints();
    const auto value = syncpoints->read(id);
    if (!value) return NvResult::BadValue;
    const auto slot = out_value.get();
    if (slot < EVENT_COUNT && events[slot].event != nullptr && timeout != 0) {
        auto& event = events[slot];
        syncpoints->cancelWait(event.wait_token);
        event.event->clear();
        event.event->retain();
        const std::shared_ptr<kernel::Event> retained(
            event.event, [](kernel::Event* ptr) { ptr->release(); });
        const auto token = syncpoints->onReached(
            id, tresh, [retained] { retained->signal(); });
        if (!token) return NvResult::BadValue;
        event.wait_token = *token;
        LOG_INFO(Services, "Video event {} waits for syncpoint {} threshold {} (current {})",
                  slot, id, tresh, *value);
        return video::Syncpoints::reached(*value, tresh)
                   ? NvResult::Success : NvResult::BadValue;
    }
    out_value = *value;
    return video::Syncpoints::reached(*value, tresh) ? NvResult::Success : NvResult::Timeout;
}

NvResult NvHostCtrl::syncptRead(IoctlContext* context, u32 id, u32* out_value) {
    const auto value = context->system.getGpu().getVideoSyncpoints().read(id);
    if (!value) return NvResult::BadValue;
    *out_value = *value;
    return NvResult::Success;
}

NvResult NvHostCtrl::syncptAllocEvent(u32 slot) {
    if (slot >= EVENT_COUNT)
        return NvResult::BadValue;
    auto& event = events[slot];
    if (syncpoints != nullptr)
        syncpoints->cancelWait(event.wait_token);
    event.wait_token = 0;

    // Check if event is already allocated
    // TODO: correct?
    if (event.event != nullptr)
        event.event->release();

    event.event = new kernel::Event(false, fmt::format("NvHostEvent {}", slot));

    return NvResult::Success;
}

NvResult NvHostCtrl::syncptFreeEvent(u32 slot) {
    if (slot >= EVENT_COUNT || events[slot].event == nullptr)
        return NvResult::BadValue;
    auto& event = events[slot];
    if (syncpoints != nullptr)
        syncpoints->cancelWait(event.wait_token);
    event.wait_token = 0;
    event.event->release();
    event.event = nullptr;

    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl

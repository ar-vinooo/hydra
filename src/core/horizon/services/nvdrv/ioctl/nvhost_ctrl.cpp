#include "core/horizon/services/nvdrv/ioctl/nvhost_ctrl.hpp"

#include "core/horizon/kernel/event.hpp"
#include "core/system.hpp"
#include "core/horizon/services/settings/nx_settings.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

namespace {

u32 eventSlot(u32 event_id) {
    return (event_id & 0x10000000u) != 0
        ? event_id & 0xffffu : event_id & 0xffu;
}

} // namespace

u64 NvHostEvent::cancelWait(video::Syncpoints* syncpoints) {
    u64 generation;
    {
        std::scoped_lock lock(wait_state->mutex);
        generation = ++wait_state->generation;
        if (event)
            event->clear();
    }
    if (syncpoints)
        syncpoints->cancelWait(wait_token);
    wait_token = 0;
    return generation;
}

bool NvHostEvent::armWait(video::Syncpoints& syncpoints, u32 id, u32 threshold) {
    const auto generation = cancelWait(&syncpoints);
    event->retain();
    const std::shared_ptr<kernel::Event> retained(
        event, [](kernel::Event* ptr) { ptr->release(); });
    const auto token = syncpoints.onReached(
        id, threshold, [retained, state = wait_state, generation] {
            std::scoped_lock lock(state->mutex);
            // Cancellation also invalidates callbacks already dequeued for delivery.
            if (state->generation == generation)
                retained->signal();
        });
    if (!token)
        return false;
    wait_token = *token;
    return true;
}

NvHostCtrl::~NvHostCtrl() {
    std::scoped_lock lock(events_mutex);
    for (auto& event : events) {
        event.cancelWait(syncpoints);
        if (event.event != nullptr)
            event.event->release();
    }
}

DEFINE_IOCTL_TABLE(NvHostCtrl,
                   DEFINE_IOCTL_TABLE_ENTRY(NvHostCtrl, 0x00, 0x14, syncptRead, 0x1b, getConfig,
                                            0x1c, syncptSignalEvent, 0x1d, syncptWaitEvent, 0x1e,
                                            syncptWaitEventEx, 0x1f,
                                            syncptAllocEvent, 0x20,
                                            syncptFreeEvent))

NvResult NvHostCtrl::queryEvent(u32 event_id_u32, kernel::Event*& out_event) {
    std::scoped_lock lock(events_mutex);
    const auto slot = eventSlot(event_id_u32);
    u32 syncpoint_id;
    if (extractBits(event_id_u32, 28, 1) != 0u) { // New format
        syncpoint_id = extractBits(event_id_u32, 16, 12);
    } else { // Old format
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

NvResult NvHostCtrl::syncptWaitEvent(IoctlContext* context, u32 id, u32 threshold, i32 timeout,
                                     u32* out_value) {
    LOG_DEBUG(Services, "Syncpoint wait: id {}, threshold {}, timeout {}",
              id, threshold, timeout);

    if (id == 0) { *out_value = 0; return NvResult::Success; } // Existing GPU path.
    auto& gpu = context->system.getGpu();
    auto& syncpoints_ref = gpu.getVideoSyncpoints();
    // A GPU fence is reached when the host finishes the work: wait for it,
    // bounded so a lost fence cannot hang the guest
    if (gpu.isFenceSyncpoint(id)) {
        const auto wait = timeout < 0 ? std::chrono::milliseconds(10000)
                                      : std::chrono::milliseconds(timeout);
        if (!syncpoints_ref.waitFor(id, threshold, wait))
            ONCE(LOG_WARN(Services, "GPU fence {}:{} not reached in {} ms", id,
                          threshold, wait.count()));
    }
    const auto value = syncpoints_ref.read(id);
    if (!value) return NvResult::BadValue;
    *out_value = *value;
    return video::Syncpoints::reached(*value, threshold) ? NvResult::Success : NvResult::Timeout;
}

NvResult NvHostCtrl::syncptWaitEventEx(IoctlContext* context, u32 id, u32 threshold, i32 timeout,
                                       InOutSingle<u32> out_value) {
    LOG_DEBUG(Services, "Async syncpoint wait: id {}, threshold {}, timeout {}",
              id, threshold, timeout);

    if (id == 0) { out_value = 0; return NvResult::Success; } // Existing GPU path.
    std::scoped_lock lock(events_mutex);
    syncpoints = &context->system.getGpu().getVideoSyncpoints();
    const auto value = syncpoints->read(id);
    if (!value) return NvResult::BadValue;
    const auto slot = out_value.get();
    if (slot < EVENT_COUNT && events[slot].event != nullptr && timeout != 0) {
        auto& event = events[slot];
        if (!event.armWait(*syncpoints, id, threshold))
            return NvResult::BadValue;
        return video::Syncpoints::reached(*value, threshold)
                   ? NvResult::Success : NvResult::Timeout;
    }
    out_value = *value;
    return video::Syncpoints::reached(*value, threshold) ? NvResult::Success : NvResult::Timeout;
}

NvResult NvHostCtrl::syncptRead(IoctlContext* context, u32 id, u32* out_value) {
    const auto value = context->system.getGpu().getVideoSyncpoints().read(id);
    if (!value) return NvResult::BadValue;
    *out_value = *value;
    return NvResult::Success;
}

NvResult NvHostCtrl::syncptSignalEvent(u32 event_id) {
    const auto slot = eventSlot(event_id);
    std::scoped_lock lock(events_mutex);
    if (slot >= EVENT_COUNT || !events[slot].event)
        return NvResult::BadValue;
    events[slot].cancelWait(syncpoints);
    return NvResult::Success;
}

NvResult NvHostCtrl::syncptAllocEvent(u32 slot) {
    std::scoped_lock lock(events_mutex);
    if (slot >= EVENT_COUNT)
        return NvResult::BadValue;
    auto& event = events[slot];
    event.cancelWait(syncpoints);

    // Check if event is already allocated
    // TODO: correct?
    if (event.event != nullptr)
        event.event->release();

    event.event = new kernel::Event(false, fmt::format("NvHostEvent {}", slot));

    return NvResult::Success;
}

NvResult NvHostCtrl::syncptFreeEvent(u32 slot) {
    std::scoped_lock lock(events_mutex);
    if (slot >= EVENT_COUNT || events[slot].event == nullptr)
        return NvResult::BadValue;
    auto& event = events[slot];
    event.cancelWait(syncpoints);
    event.event->release();
    event.event = nullptr;

    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl

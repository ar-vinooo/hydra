#include "core/horizon/services/hid/internal/applet_resource.hpp"

#include "core/horizon/kernel/shared_memory.hpp"
#include "core/system.hpp"

#define SHARED_MEMORY (*reinterpret_cast<SharedMemory*>(shared_mem->getPtr()))

namespace hydra::horizon::services::hid::internal {

#define NPAD_INTERNAL_STATE(i)                                                 \
    Npad { SHARED_MEMORY.npad.entries[i].internal_state }

AppletResource::AppletResource(System& system)
    : shared_mem{new kernel::SharedMemory(system.getCpu(), sizeof(SharedMemory),
                                          "HID shared memory")},
      npads{{NPAD_INTERNAL_STATE(0), NPAD_INTERNAL_STATE(1),
             NPAD_INTERNAL_STATE(2), NPAD_INTERNAL_STATE(3),
             NPAD_INTERNAL_STATE(4), NPAD_INTERNAL_STATE(5),
             NPAD_INTERNAL_STATE(6), NPAD_INTERNAL_STATE(7),
             NPAD_INTERNAL_STATE(8), NPAD_INTERNAL_STATE(9)}} {
    // SharedMemory allocates zeroed bytes; construct the HID layout so each
    // ring header gets its capacity and the guest can read valid samples.
    ::new (reinterpret_cast<void*>(shared_mem->getPtr())) SharedMemory{};

    for (auto& npad : npads)
        npad.update({});

    // Readers copy nothing from an empty lifo, leaving their output
    // uninitialized, so always provide a "no touches" sample
    updateTouch({});
    // These devices are not exposed to the guest yet. A neutral sample lets
    // unconditional SDK reads return initialized data instead of no state.
    SHARED_MEMORY.keyboard.lifo.writeNext({});
    SHARED_MEMORY.mouse.lifo.writeNext({});
    SHARED_MEMORY.debug_pad.lifo.writeNext({});
}

#undef NPAD_INTERNAL_STATE

AppletResource::~AppletResource() { shared_mem->release(); }

void AppletResource::activateNpads(NpadRevision revision) {
    active = true;
    (void)revision;

    // TODO: revision affects style sets?
    LOG_DEBUG(Services, "Npad activation with {}", revision);

    setupNpads();
}

void AppletResource::setupNpads() {
    for (u32 i = 0; i < NPAD_COUNT; i++) {
        // TODO: get this from the config
        NpadStyleSet style_set;
        switch (i) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            style_set = NpadStyleSet::FullKey;
            break;
        case 8:
            style_set = NpadStyleSet::Handheld;
            break;
        case 9:
            style_set = NpadStyleSet::Palma;
            break;
        default:
            style_set = NpadStyleSet::None;
            break;
        }

        // Disable handheld npads in docked mode
        if (style_set == NpadStyleSet::Handheld &&
            !CONFIG_INSTANCE.getHandheldMode())
            style_set = NpadStyleSet::None;

        // TODO: auto-remap ProController to JoyDual and vice-versa

        // Check if supported
        const auto& profiles = CONFIG_INSTANCE.getInputProfiles();
        if (i >= profiles.size() || profiles[i].empty() ||
            !supported_npads[i] || !any(supported_style_sets & style_set))
            style_set = NpadStyleSet::None;

        npads[i].setup(style_set);
    }
}

void AppletResource::updateTouch(
    const std::map<u32, input::TouchState>& new_state) {
    auto& lifo = SHARED_MEMORY.touch_screen.lifo;

    // Like the real HID service, keep sampling even when input is ignored, as
    // games read the touch screen unconditionally
    const bool accept = shouldAcceptInput();

    // State
    TouchScreenState state{};
    u32 index = 0;
    for (const auto& [finger_id, touch_state] : new_state) {
        if (!accept || index >= std::size(state.touches))
            break;
        state.touches[index++] = {
            .finger_id = finger_id,
            .x = touch_state.x,
            .y = touch_state.y,
            // TODO: more
        };
    }
    state.count = static_cast<i32>(index);
    lifo.writeNext(state);
}

} // namespace hydra::horizon::services::hid::internal

#include "core/input/device_manager.hpp"
#include "core/input/player_assignment.hpp"
#include "core/input/profile.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef ZTD_PLATFORM_APPLE
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace hydra;
using namespace hydra::input;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        fmt::format(
            "hydra-input-test-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        PlayerDevices players;
        players[0] = {"Generic Keyboard", "Pro Controller"};
        players[1] = {"Pro Controller (2)"};
        const std::vector<std::string> selected{"Pro Controller",
                                                "Pro Controller", ""};
        check(assignPlayerDevices(players, 1, selected), "Assign Player 2");
        check(players[0] == std::vector<std::string>{"Generic Keyboard"},
              "Move controller while preserving keyboard");
        check(players[1] == std::vector<std::string>{"Pro Controller"},
              "Deduplicate selection");
        const std::vector<std::string> second{"Pro Controller (2)"};
        assignPlayerDevices(players, 2, second);
        check(players[1].size() == 1 && players[2] == second,
              "Identical model controllers remain distinct");
        const auto before = players;
        check(!assignPlayerDevices(players, 8, selected) && players == before,
              "Reject invalid player without mutation");
        assignPlayerDevices(players, 1, players[1]);
        check(players == before, "Self assignment does not erase devices");
        assignPlayerDevices(players, 1, {});
        check(players[1].empty() && players[2] == second,
              "Disconnect only selected player");

        std::filesystem::create_directory(directory);
        const std::string fixture =
            R"(devices = ["Generic Keyboard", "Pro Controller"]
[Buttons]
a = ["Controller/B", "Keyboard/L"]
b = ["Controller/A"]
[AnalogSticks]
l_right = ["Controller/stick L right"]
)";
        {
            std::ofstream file(directory / "Default.toml");
            file << fixture;
        }
        using horizon::services::hid::internal::NpadIndex;
        Profile source(NpadIndex::No1, "Default", directory.string());
        check(source.getButtonMappings().size() == 3,
              "Read custom face button mappings");
        auto clone = source;
        check(clone.saveAs("Hydra Player 2", second),
              "Save dedicated player profile");
        Profile restored(NpadIndex::No2, "Hydra Player 2", directory.string());
        check(restored.getDeviceNames() == second, "Persist assigned device");
        check(restored.getButtonMappings().size() ==
                  source.getButtonMappings().size(),
              "Preserve button bindings");
        for (usize i = 0; i < restored.getButtonMappings().size(); i++) {
            const auto& a = restored.getButtonMappings()[i];
            // TOML table iteration order may differ, compare bindings by value.
            bool found = false;
            for (const auto& b : source.getButtonMappings())
                if (a.code.device_type == b.code.device_type &&
                    a.code.value == b.code.value &&
                    a.npad_buttons == b.npad_buttons)
                    found = true;
            check(found, "Preserve custom face button binding values");
        }
        check(restored.getAnalogMappings().size() == 1,
              "Preserve analog bindings");
        std::ifstream file(directory / "Default.toml");
        const std::string unchanged((std::istreambuf_iterator<char>(file)), {});
        check(unchanged == fixture, "Keep original profile unchanged");
        std::filesystem::remove_all(directory);
        if (argc == 2 && std::string_view(argv[1]) == "--connected-devices") {
#ifdef ZTD_PLATFORM_APPLE
            // The backend discovers devices asynchronously through the main
            // run loop; a command-line process has no AppKit event loop.
            for (u32 discovery = 0; discovery < 20; discovery++) {
                (void)DeviceManager::getAvailableControllerNames();
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
            }
#endif
            // Exercise the same observer constructor/destructor used by
            // Settings refresh, with the currently attached physical devices.
            for (u32 refresh = 0; refresh < 30; refresh++) {
                const auto names = DeviceManager::getAvailableControllerNames();
                check(!names.empty(), "No connected input devices to test");
                if (refresh == 0)
                    for (const auto& name : names)
                        std::cout << "Detected: " << name << '\n';
            }
            std::cout << "30 connected-device refresh cycles passed\n";
        }
        std::cout << "Player assignment and profile persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}

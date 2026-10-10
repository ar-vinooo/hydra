#pragma once

#include "core/horizon/services/hid/internal/npad_index.hpp"
#include "core/input/const.hpp"

namespace hydra::input {

class Profile {
  public:
    Profile(horizon::services::hid::internal::NpadIndex index_,
            std::string_view name_, std::string_view directory_ = {});

    void reset() {
        button_mappings = {};
        analog_mappings = {};
    }

    void loadDefaults();

    bool serialize();
    void deserialize();

    bool saveAs(std::string_view new_name, std::vector<std::string> devices) {
        name = new_name;
        device_names = std::move(devices);
        return serialize();
    }

  private:
    horizon::services::hid::internal::NpadIndex index;
    std::string name;
    std::string directory;

    std::vector<std::string> device_names;
    std::vector<CodeButtonMapping> button_mappings;
    std::vector<CodeAnalogMapping> analog_mappings;

    // Helpers
    static std::string getProfilesPath() {
        return fmt::format("{}/input_profiles",
                           CONFIG_INSTANCE.getAppDataPath());
    }

    std::string getPath() const {
        return fmt::format("{}/{}.toml",
                           directory.empty() ? getProfilesPath() : directory,
                           name);
    }

  public:
    CONST_REF_GETTER(device_names, getDeviceNames);
    CONST_REF_GETTER(button_mappings, getButtonMappings);
    CONST_REF_GETTER(analog_mappings, getAnalogMappings);
};

} // namespace hydra::input

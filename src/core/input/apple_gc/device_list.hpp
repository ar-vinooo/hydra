#pragma once

#include "core/input/device_list.hpp"

namespace hydra::input::apple_gc {

class DeviceList : public IDeviceList {
  public:
    DeviceList();
    ~DeviceList() override;

    void addController(id controller);
    void removeController(id controller);
    void addKeyboard(id keyboard);
    void removeKeyboard(id keyboard);

  private:
    std::mutex controller_mutex;
    std::map<id, std::string> controller_names;
    // Creating the observer enumerates connected controllers immediately and
    // calls addController(). Its mutex/map must already be constructed.
    id impl;
};

} // namespace hydra::input::apple_gc

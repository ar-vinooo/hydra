#pragma once

#include "core/horizon/kernel/resource_limit.hpp"
#include "core/horizon/kernel/synchronization_object.hpp"

namespace hydra::horizon::kernel {

// TODO: ReadableEvent and WritableEvent
class Event : public SynchronizationObject {
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::Event;

    explicit Event(bool signalled = false,
                   std::string_view debug_name = "Event")
        : SynchronizationObject(TYPE_ID, signalled, debug_name) {}

    // Events from svc::CreateEvent count against the process's limit
    void chargeTo(ResourceLimit* limit) {
        charge.charge(limit, LimitableResource::EventCountMax);
    }

  private:
    ResourceCharge charge;
};

} // namespace hydra::horizon::kernel

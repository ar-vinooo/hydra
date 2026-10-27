#pragma once

#include "core/horizon/kernel/auto_object.hpp"

namespace hydra::horizon::kernel {

enum class LimitableResource : u32 {
    PhysicalMemoryMax = 0,
    ThreadCountMax = 1,
    EventCountMax = 2,
    TransferMemoryCountMax = 3,
    SessionCountMax = 4,

    Count,
};

// The limits, current values and peaks of a process's resources. Hydra
// reports these but does not refuse resources past a limit.
class ResourceLimit : public AutoObject {
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::ResourceLimit;

    explicit ResourceLimit(std::string_view debug_name = "Resource limit")
        : AutoObject(TYPE_ID, debug_name) {}

    void setLimitValue(LimitableResource which, i64 value) {
        std::scoped_lock lock(mutex);
        values[index(which)].limit = value;
    }
    void add(LimitableResource which, i64 count = 1) {
        std::scoped_lock lock(mutex);
        auto& value = values[index(which)];
        value.current += count;
        value.peak = std::max(value.peak, value.current);
    }
    void remove(LimitableResource which, i64 count = 1) {
        std::scoped_lock lock(mutex);
        values[index(which)].current -= count;
    }
    // For resources measured rather than counted, like memory
    void setCurrentValue(LimitableResource which, i64 current) {
        std::scoped_lock lock(mutex);
        auto& value = values[index(which)];
        value.current = current;
        value.peak = std::max(value.peak, value.current);
    }

    i64 getLimitValue(LimitableResource which) {
        std::scoped_lock lock(mutex);
        return values[index(which)].limit;
    }
    i64 getCurrentValue(LimitableResource which) {
        std::scoped_lock lock(mutex);
        return values[index(which)].current;
    }
    i64 getPeakValue(LimitableResource which) {
        std::scoped_lock lock(mutex);
        return values[index(which)].peak;
    }

  private:
    struct Value {
        i64 limit{0};
        i64 current{0};
        i64 peak{0};
    };

    std::mutex mutex;
    std::array<Value, static_cast<usize>(LimitableResource::Count)> values{};

    static usize index(LimitableResource which) {
        return static_cast<usize>(which);
    }
};

// A resource an object holds until it is destroyed
class ResourceCharge {
  public:
    ResourceCharge() = default;
    ~ResourceCharge() {
        if (limit != nullptr) {
            limit->remove(which);
            limit->release();
        }
    }
    ZTD_MAKE_NON_COPYABLE(ResourceCharge);
    ZTD_MAKE_NON_MOVABLE(ResourceCharge);

    void charge(ResourceLimit* limit_, LimitableResource which_) {
        ASSERT_DEBUG(limit == nullptr, Kernel, "Resource charged twice");
        limit = limit_;
        which = which_;
        limit->retain();
        limit->add(which);
    }

  private:
    ResourceLimit* limit{nullptr};
    LimitableResource which{LimitableResource::Count};
};

} // namespace hydra::horizon::kernel

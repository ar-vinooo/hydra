#pragma once

#include "core/horizon/kernel/const.hpp"

namespace hydra::horizon::kernel {

class SynchronizationObject;

enum class AutoObjectTypeId {
    ClientPort,
    ClientSession,
    Port,
    ServerPort,
    ServerSession,
    Session,
    CodeMemory,
    Event, // TODO: ReadableEvent and WritableEvent
    Process,
    SharedMemory,
    Thread,
    TransferMemory,
};

class AutoObject {
  public:
    explicit AutoObject(
        AutoObjectTypeId type_id_,
        const std::string_view debug_name_ = "AutoObject") noexcept
        : type_id{type_id_},
          debug_name{fmt::format("{} {}", debug_name_,
                                 reinterpret_cast<void*>(this))} {}
    virtual ~AutoObject() noexcept = default;

    ZTD_MAKE_NON_COPYABLE(AutoObject);
    ZTD_MAKE_NON_MOVABLE(AutoObject);

    void retain() { ref_count.fetch_add(1, std::memory_order_relaxed); }

    // Returns true if the object has been deallocated
    bool release() {
        if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // TODO: this assumes that the object is heap allocated, but that
            // may not always be the case
            delete this;
            return true;
        }

        return false;
    }

    // Base classes without a type ID match every type derived from them
    template <typename T>
    bool isOfType() const
        requires std::is_base_of_v<AutoObject, T>
    {
        if constexpr (std::is_same_v<T, AutoObject>) {
            return true;
        } else if constexpr (std::is_same_v<T, SynchronizationObject>) {
            switch (type_id) {
            case AutoObjectTypeId::ClientPort:
            case AutoObjectTypeId::ServerPort:
            case AutoObjectTypeId::ServerSession:
            case AutoObjectTypeId::Event:
            case AutoObjectTypeId::Process:
            case AutoObjectTypeId::Thread:
                return true;
            default:
                return false;
            }
        } else {
            return type_id == T::TYPE_ID;
        }
    }

    // nullptr unless the object is a T (there is no RTTI)
    template <typename T>
    T* as() {
        return isOfType<T>() ? static_cast<T*>(this) : nullptr;
    }

    std::string_view getDebugName() const { return debug_name; }

  private:
    AutoObjectTypeId type_id;
    std::string debug_name;

    std::atomic<u32> ref_count{1};

  public:
    GETTER(type_id, getTypeId);
};

} // namespace hydra::horizon::kernel

ENABLE_ENUM_FORMATTING(hydra::horizon::kernel::AutoObjectTypeId, ClientPort,
                       "client port", ClientSession, "client session", Port,
                       "port", ServerPort, "server port", ServerSession,
                       "server session", Session, "session", CodeMemory,
                       "code memory", Event, "event", Process, "process",
                       SharedMemory, "shared memory", Thread, "thread",
                       TransferMemory, "transfer memory")

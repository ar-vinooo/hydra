#pragma once

#include "core/horizon/kernel/hipc/const.hpp"
#include "core/horizon/kernel/synchronization_object.hpp"

namespace hydra::horizon::kernel {
class Event;
class Process;
} // namespace hydra::horizon::kernel

namespace hydra::horizon::kernel::hipc {

class Session;

constexpr u64 MSG_BUFFER_MAX_SIZE = 0x2000; // TODO: what should this be?

struct SessionRequest {
    Process* client_process;
    IThread* client_thread;
    // The client's message buffer: its TLS, or a user buffer
    uptr ptr;
    u64 size;
    // Signalled instead of resuming the client for asynchronous requests
    Event* reply_event;
};

// TODO: should maintain a reference to the parent session
class ServerSession : public SynchronizationObject {
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::ServerSession;

    explicit ServerSession(std::string_view debug_name = "Server session")
        : SynchronizationObject(TYPE_ID, false, debug_name) {}
    ~ServerSession() override;

    void onClientClose();

    bool isClientOpen() {
        std::scoped_lock lock(mutex);
        return client_open;
    }

    // Server
    void receive(IThread* crnt_thread);
    void receive(uptr ptr, u64 size);
    void reply(uptr ptr, u64 size = MSG_BUFFER_MAX_SIZE);

    bool hasRequests() {
        std::scoped_lock lock(mutex);
        return !requests.empty();
    }

    // HACK
    kernel::Process* getActiveRequestClientProcess() {
        std::scoped_lock lock(mutex);
        return active_request->client_process;
    }

    // Client
    void enqueueRequest(Process* client_process, IThread* client_thread,
                        uptr ptr, u64 size = MSG_BUFFER_MAX_SIZE,
                        Event* reply_event = nullptr);

  private:
    Session* parent{nullptr};

    bool client_open{true};

    std::mutex mutex;
    std::queue<SessionRequest> requests;
    std::optional<SessionRequest> active_request;

  public:
    GETTER_AND_SETTER(parent, getParent, setParent);
};

} // namespace hydra::horizon::kernel::hipc

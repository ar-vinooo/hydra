#include "core/horizon/kernel/hipc/server_session.hpp"

#include "core/horizon/kernel/event.hpp"
#include "core/horizon/kernel/hipc/session.hpp"
#include "core/horizon/kernel/thread.hpp"

namespace hydra::horizon::kernel::hipc {

ServerSession::~ServerSession() {
    // Resume the clients of the active and queued requests
    const auto abort = [](const SessionRequest& request) {
        if (request.reply_event != nullptr)
            request.reply_event->release();
        else
            request.client_thread->abortWait();
    };
    if (active_request)
        abort(*active_request);
    for (; !requests.empty(); requests.pop())
        abort(requests.front());

    parent->onServerClose();
}

void ServerSession::onClientClose() {
    std::scoped_lock lock(mutex);
    client_open = false;

    // Signal the server that client has closed
    signal();
}

void ServerSession::receive(IThread* crnt_thread) {
    receive(crnt_thread->getTlsPtr(), MSG_BUFFER_MAX_SIZE);
}

void ServerSession::receive(uptr ptr, u64 size) {
    std::scoped_lock lock(mutex);
    ASSERT_DEBUG(!requests.empty(), Kernel, "No requests");
    active_request = requests.front();
    requests.pop();

    // Copy the message to the server's buffer
    std::memcpy(reinterpret_cast<void*>(ptr),
                reinterpret_cast<void*>(active_request->ptr),
                std::min(active_request->size, size));

    // Clear if no more requests
    if (requests.empty())
        clear();
}

void ServerSession::reply(uptr ptr, u64 size) {
    std::scoped_lock lock(mutex);

    // Copy the message to the client's buffer
    memcpy(reinterpret_cast<void*>(active_request->ptr),
           reinterpret_cast<void*>(ptr), std::min(active_request->size, size));

    // Resume the client thread, or signal its asynchronous request
    if (auto event = active_request->reply_event) {
        event->signal();
        event->release();
    } else {
        active_request->client_thread->resume();
    }

    active_request = std::nullopt;
}

void ServerSession::enqueueRequest(Process* client_process,
                                   IThread* client_thread, uptr ptr, u64 size,
                                   Event* reply_event) {
    std::scoped_lock lock(mutex);
    size = std::min(size, MSG_BUFFER_MAX_SIZE);
    if (reply_event != nullptr)
        reply_event->retain();
    requests.push({.client_process = client_process,
                   .client_thread = client_thread,
                   .ptr = ptr,
                   .size = size,
                   .reply_event = reply_event});

    // Signal the server to process the request
    signal();
}

} // namespace hydra::horizon::kernel::hipc

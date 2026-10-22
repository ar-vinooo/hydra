#pragma once

#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <span>

#include "common/types.hpp"
#include "core/horizon/online/online.hpp"

namespace hydra::horizon::services::socket {

// What one nn::socket call returns: its return value and errno (nn::socket
// uses Linux errno values)
struct CallResult {
    i32 ret;
    u32 error;
};

// The sockets of a title that plays online, as host sockets. The host sockets
// never block: a call on a socket the title made blocking waits in poll, which
// interrupt() ends.
class HostSockets {
  public:
    HostSockets();
    ~HostSockets();

    // Ends the waiting calls, for good (emulation stops)
    void interrupt();
    void closeAll();

    CallResult open(const online::Config& config, u32 domain, u32 type,
                    u32 protocol);
    CallResult close(i32 fd);
    CallResult bind(i32 fd, std::span<const u8> addr);
    CallResult connect(i32 fd, std::span<const u8> addr);
    CallResult listen(i32 fd, i32 backlog);
    CallResult accept(i32 fd, std::span<u8> out_addr, u32& out_addr_size);
    // out_addr_size: nullptr for recv
    CallResult recv(i32 fd, std::span<u8> buffer, i32 flags,
                    std::span<u8> out_addr, u32* out_addr_size);
    // addr: empty for send
    CallResult send(i32 fd, std::span<const u8> buffer, i32 flags,
                    std::span<const u8> addr);
    CallResult getSockName(i32 fd, std::span<u8> out_addr,
                           u32& out_addr_size);
    CallResult getPeerName(i32 fd, std::span<u8> out_addr,
                           u32& out_addr_size);
    CallResult setSockOpt(i32 fd, i32 level, i32 name,
                          std::span<const u8> value);
    CallResult getSockOpt(i32 fd, i32 level, i32 name,
                          std::span<u8> out_value, u32& out_size);
    CallResult fcntl(i32 fd, i32 cmd, i32 arg);
    CallResult ioctl(i32 fd, u32 request, std::span<const u8> in,
                     std::span<u8> out);
    CallResult shutdown(i32 fd, i32 how);
    // fds: the title's pollfd array, revents written in place
    CallResult poll(std::span<u8> fds, i32 timeout_ms);
    // The fd sets are bitmaps, updated in place. timeout_ms: -1 for none.
    CallResult select(i32 nfds, std::span<u8> read_fds,
                      std::span<u8> write_fds, std::span<u8> except_fds,
                      i64 timeout_ms);

    // For nn::ssl
    bool exists(i32 fd) { return get(fd) != nullptr; }
    // Connected to the online server's HTTP port: TLS on it passes through
    bool isServerHttp(i32 fd);

  private:
    struct Socket {
        int host_fd;
        bool is_stream;
        bool non_blocking;
        const online::Config* config;
        i64 recv_timeout_ms{-1};
        i64 send_timeout_ms{-1};
        std::optional<online::HttpProofInjector> http;
    };

    std::map<i32, Socket> sockets;
    i32 next_fd{3};
    std::array<int, 2> wake_pipe{-1, -1};
    std::atomic<bool> interrupted{false};
    std::set<u64> logged_destinations;

    Socket* get(i32 fd);
    i32 add(Socket socket);
    CallResult wait(const Socket& socket, short events, i64 timeout_ms);
    template <typename Op>
    CallResult run(Socket& socket, short events, bool dont_wait,
                   i64 timeout_ms, Op op);
    CallResult sendAll(Socket& socket, std::span<const u8> data);
};

} // namespace hydra::horizon::services::socket

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace hydra::horizon::online {

// A title's online settings, <app data>/online/<title ID>.toml (written by the
// frontend's "Configure Online..."). Without the file, or with enabled = false,
// the network services stay offline.
//
// The model is OCTR's (the online fork of the CTR Nitro-Fueled PC port) for
// RyuNET, its Demonware server: the Demonware hosts resolve to the server, the
// HTTPS requests reach its HTTP port as plain HTTP, signed with the RyuNET
// proof, and the auth replies are checked against the server's RSA key.
struct Config {
    bool enabled{false};
    // Host name or IPv4 address of the server
    std::string server;
    // The server's HTTP port: TCP connections to the server's ports 80 and 443
    // go here, without TLS
    u16 http_port{8080};
    // Also the Nintendo Account ID token the title sends (OCTR sends it as the
    // PSN ID token)
    std::string nickname;
    // Host patterns ("*.demonware.net") whose lookups return the server
    std::vector<std::string> redirect_hosts;
    // Hex modulus of the server's RSA-2048 key for Demonware auth replies;
    // empty: the title keeps its own key
    std::string demonware_key;
};

// Reads the title's settings again (called once its main module is loaded)
// and patches the Demonware key in the module's read-only data
void loadTitle(u64 title_id, std::span<u8> main_rodata);

// The title's settings when it plays online, nullptr otherwise
const Config* getConfig(u64 title_id);

// The server's IPv4 address in network byte order, 0 when it does not resolve
u32 getServerAddress(const Config& config);

// This machine's IPv4 address on the way to the server, in network byte order
u32 getLocalAddress(const Config& config);

bool isRedirectedHost(const Config& config, std::string_view host);

// The account ID OCTR derives from the nickname, so that the same nickname
// is the same account on every port
u64 getAccountId(const Config& config);

// The X-RyuNET-Proof header value for one request to the server
std::string makeProof(const Config& config, std::string_view method,
                      std::string_view target);

// Adds X-RyuNET-Proof to the HTTP requests of one connection to the server.
// The title writes the requests in any pieces; each head goes out complete.
class HttpProofInjector {
  public:
    explicit HttpProofInjector(const Config& config_) : config{config_} {}

    // The bytes to send for these bytes of the title's
    std::string feed(std::span<const u8> data);

  private:
    const Config& config;

    std::string head;
    u64 body_remaining{0};
    bool raw{false}; // Not HTTP that can be followed: passed through
};

} // namespace hydra::horizon::online

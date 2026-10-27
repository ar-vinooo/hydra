#pragma once

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace hydra::horizon::services::socket::resolver {

// sfdnsres swaps even the sockaddr fields that are already in network order.
// nn::socket/libnx reverses this extra swap when decoding the reply.
inline std::array<std::uint8_t, 16>
serializeIpv4SockAddr(std::uint32_t network_address,
                     std::uint16_t network_port) {
    std::array<std::uint8_t, 16> data{};
    data[1] = 2; // Guest AF_INET, serialized family
    const auto port = ntohs(network_port);
    const auto address = ntohl(network_address);
    std::memcpy(data.data() + 2, &port, sizeof(port));
    std::memcpy(data.data() + 4, &address, sizeof(address));
    return data;
}

} // namespace hydra::horizon::services::socket::resolver

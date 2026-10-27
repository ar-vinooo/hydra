#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "core/horizon/services/socket/resolver/address_serialization.hpp"

using hydra::horizon::services::socket::resolver::serializeIpv4SockAddr;

int main() {
    for (const auto* ip : {"165.1.125.122", "127.0.0.1", "192.168.1.20"}) {
        in_addr address{};
        if (inet_pton(AF_INET, ip, &address) != 1)
            throw std::runtime_error("Invalid fixture");
        for (const auto port : std::array<std::uint16_t, 4>{0, 80, 443, 3074}) {
            const auto wire = serializeIpv4SockAddr(address.s_addr, htons(port));
            std::uint16_t decoded_port;
            std::uint32_t decoded_address;
            std::memcpy(&decoded_port, wire.data() + 2, sizeof(decoded_port));
            std::memcpy(&decoded_address, wire.data() + 4, sizeof(decoded_address));
            // Match libnx _resolverDeserializeAddrInfo's extra byte swaps.
            decoded_port = ntohs(decoded_port);
            decoded_address = ntohl(decoded_address);
            if (wire[0] != 0 || wire[1] != 2 ||
                decoded_port != htons(port) || decoded_address != address.s_addr)
                throw std::runtime_error("Resolver changed the IP or port");
        }
    }
    in_addr address{};
    inet_pton(AF_INET, "165.1.125.122", &address);
    const auto wire = serializeIpv4SockAddr(address.s_addr, htons(3074));
    const std::array<std::uint8_t, 8> expected{0, 2, 2, 12, 122, 125, 1, 165};
    if (!std::equal(expected.begin(), expected.end(), wire.begin()))
        throw std::runtime_error("Incorrect sfdnsres wire bytes");
    std::cout << "PASS: resolver preserves IPv4 addresses and ports through guest decoding\n";
}

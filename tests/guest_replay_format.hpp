#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <stdexcept>
#include <utility>
#include <vector>

// Keep this transport aligned with tools/guest_event_format.py. Decode fields
// explicitly; no native struct layout or endianness enters the file format.
namespace hydra::guest_replay {
enum class Kind : std::uint32_t {
    SessionBegin,
    SessionEnd,
    Map,
    Unmap,
    ChannelSwitch,
    ChannelDestroy,
    AddressSpaceDestroy,
    PfifoEntry,
    PfifoWord,
    Method,
    MacroFlush,
    SemaphoreWait,
    SemaphoreResume,
    SyncpointWait,
    SyncpointResume,
    SemaphoreAcquire,
    SemaphoreAcquired,
    SemaphoreRelease,
    SemaphoreReleaseQueued,
    MemoryInitial,
    MemoryWrite,
    MemoryPublished,
    MemoryWriteback,
    GpuWriteIssued,
    GpuComplete,
    ReportQueued,
    ReportComplete,
    ResourceRead,
    ResourceWrite,
    ResourceInvalidate,
    Checkpoint,
    Draw,
    Clear,
    Blit,
    Compute,
    DmaAccess,
    DmaCopy,
    ChannelFault,
    SyncpointInitial,
    NativeSubmit,
    NativeComplete,
    Count
};
struct Event {
    Kind kind;
    std::uint64_t order, channel, sequence, source, address, size, version,
        value, auxiliary;
    std::vector<std::uint8_t> data;
};

inline std::uint64_t readInteger(std::istream& input, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < width; ++byte) {
        const auto next = input.get();
        if (next == std::char_traits<char>::eof())
            throw std::runtime_error("truncated native guest event");
        value |= std::uint64_t(static_cast<unsigned char>(next)) << (byte * 8);
    }
    return value;
}

inline std::vector<Event> readEvents(std::istream& input,
                                     std::uint64_t budget) {
    constexpr std::array<char, 8> magic{'H', 'Y', 'D', 'R',
                                        'A', 'E', '1', '\n'};
    std::array<char, 8> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!input || header != magic)
        throw std::runtime_error("invalid native guest event header");
    std::vector<Event> events;
    std::uint64_t used = header.size();
    while (input.peek() != std::char_traits<char>::eof()) {
        if (budget - std::min(budget, used) < sizeof(Event) + 80)
            throw std::runtime_error("native event memory budget exceeded");
        const auto kind = readInteger(input, 4);
        const auto length = readInteger(input, 4);
        if (kind >= static_cast<unsigned>(Kind::Count) || length > 65536)
            throw std::runtime_error(
                "unsupported native event or oversized payload");
        Event event{};
        event.kind = static_cast<Kind>(kind);
        for (auto* field : {&event.order, &event.channel, &event.sequence,
                            &event.source, &event.address, &event.size,
                            &event.version, &event.value, &event.auxiliary})
            *field = readInteger(input, 8);
        used += sizeof(Event) + 80;
        if (length > budget - std::min(budget, used))
            throw std::runtime_error("native payload memory budget exceeded");
        used += length;
        event.data.resize(length);
        input.read(reinterpret_cast<char*>(event.data.data()),
                   static_cast<std::streamsize>(length));
        if (!input || event.order != events.size() + 1)
            throw std::runtime_error(
                "truncated or unordered native guest event");
        if ((!events.empty() && events.back().kind == Kind::SessionEnd) ||
            (event.kind == Kind::SessionBegin && !events.empty()))
            throw std::runtime_error(
                "native stream contains multiple sessions");
        const bool memory = event.kind == Kind::MemoryInitial ||
                            event.kind == Kind::MemoryWrite ||
                            event.kind == Kind::MemoryPublished ||
                            event.kind == Kind::MemoryWriteback;
        if ((memory && (length == 0 || event.size != length)) ||
            (!memory && length != 0))
            throw std::runtime_error(
                "native event payload does not match its kind or range");
        events.push_back(std::move(event));
    }
    if (events.size() < 2 || events.front().kind != Kind::SessionBegin ||
        events.front().value != 1 || events.back().kind != Kind::SessionEnd ||
        events.back().value != 1 || events.back().size ||
        events.back().version || events.back().auxiliary)
        throw std::runtime_error("native replay requires a complete capture");
    return events;
}
} // namespace hydra::guest_replay

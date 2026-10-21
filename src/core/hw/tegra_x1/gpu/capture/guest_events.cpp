#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"

#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/pfifo.hpp"
#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>

namespace hydra::hw::tegra_x1::gpu::capture {

GuestEventOrigin currentGuestEventOrigin() {
    const auto* state = tls_crnt_decode_state;
    return state ? GuestEventOrigin{state->channel_id, state->active_sequence,
                                    state->dispatch_source_address}
                 : GuestEventOrigin{};
}

GuestEvents& GuestEvents::instance() {
    // Completion callbacks can outlive the owning Gpu; never leave them a
    // dangling recorder. finish() explicitly closes the recording.
    static auto* events = new GuestEvents;
    return *events;
}

GuestEvents::GuestEvents() {
    const auto* path = std::getenv("HYDRA_GUEST_EVENTS");
    if (!path || !*path)
        return;
    if (const auto* budget = std::getenv("HYDRA_GUEST_EVENTS_LIMIT_MB")) {
        std::uint64_t mb = 0;
        const std::string_view text(budget);
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), mb);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || mb == 0 ||
            mb > std::numeric_limits<std::uint64_t>::max() / (1ull << 20)) {
            std::cerr << "[GuestEvents] Invalid HYDRA_GUEST_EVENTS_LIMIT_MB\n";
            return;
        }
        limit = mb << 20;
    }
    // Avoid reusing an existing capture. A new path is required for a new run.
    const std::filesystem::path destination(path);
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error) {
        std::cerr << "[GuestEvents] Capture requires an unused output path\n";
        return;
    }
    output.open(destination, std::ios::out | std::ios::binary);
    if (!output) {
        std::cerr << "[GuestEvents] Cannot open capture output\n";
        return;
    }
    active.store(true, std::memory_order_release);
    writeLocked("session_begin", {}, 0, 0, 0, 1, 0);
}

void GuestEvents::truncateLocked(std::string_view reason) {
    if (!active.exchange(false, std::memory_order_acq_rel))
        return;
    output << "{\"schema\":1,\"order\":" << ++order
           << ",\"event\":\"truncated\",\"reason\":\"" << reason << "\"}\n";
    output.flush();
    output.close();
    std::cerr << "[GuestEvents] Recording truncated: " << reason << '\n';
}

void GuestEvents::writeLocked(std::string_view kind, GuestEventOrigin origin,
                              std::uint64_t address, std::uint64_t size,
                              std::uint64_t version, std::uint64_t value,
                              std::uint64_t auxiliary,
                              std::span<const std::uint8_t> data) {
    if (!active.load(std::memory_order_relaxed))
        return;
    // Event names are internal literals. No guest strings enter this format.
    std::ostringstream line;
    line << "{\"schema\":1,\"order\":" << order + 1 << ",\"event\":\"" << kind
         << "\",\"channel\":" << origin.channel
         << ",\"sequence\":" << origin.sequence
         << ",\"source\":" << origin.source << ",\"address\":" << address
         << ",\"size\":" << size << ",\"version\":" << version
         << ",\"value\":" << value << ",\"auxiliary\":" << auxiliary;
    if (!data.empty()) {
        constexpr char hex[] = "0123456789abcdef";
        line << ",\"data\":\"";
        for (const auto byte : data)
            line << hex[byte >> 4] << hex[byte & 15];
        line << '"';
    }
    line << "}\n";
    const auto text = line.str();
    if (text.size() > limit - std::min(bytes, limit)) {
        truncateLocked("byte_budget");
        return;
    }
    output << text;
    output.flush();
    if (!output) {
        std::cerr << "[GuestEvents] Recording stopped after an output error\n";
        active.store(false, std::memory_order_release);
        output.close();
        return;
    }
    ++order;
    bytes += text.size();
}

void GuestEvents::event(std::string_view kind, GuestEventOrigin origin,
                        std::uint64_t address, std::uint64_t size,
                        std::uint64_t version, std::uint64_t value,
                        std::uint64_t auxiliary) {
    if (!enabled())
        return;
    std::scoped_lock lock(mutex);
    if (kind == "gpu_write_issued")
        pending_writes[version]++;
    else if (kind == "gpu_complete")
        pending_writes.erase(version);
    else if (kind == "native_submit")
        pending_native.insert(version);
    else if (kind == "native_complete")
        pending_native.erase(version);
    writeLocked(kind, origin, address, size, version, value, auxiliary);
}

void GuestEvents::memory(std::string_view kind, std::uintptr_t address,
                         std::uint64_t size, std::uint64_t version,
                         GuestEventOrigin origin) {
    if (!enabled() || size == 0)
        return;
    if (address == 0 ||
        size > std::numeric_limits<std::uintptr_t>::max() - address) {
        std::scoped_lock lock(mutex);
        truncateLocked("invalid_memory_range");
        return;
    }
    constexpr std::uint64_t CHUNK = 64 * 1024;
    for (std::uint64_t offset = 0; offset < size && enabled();) {
        const auto count = std::min(CHUNK, size - offset);
        // Reading protected guest memory can cause a GPU writeback and a
        // completion callback. Never hold the recorder mutex across that read.
        const auto* begin =
            reinterpret_cast<const std::uint8_t*>(address + offset);
        const std::vector<std::uint8_t> snapshot(begin, begin + count);
        std::scoped_lock lock(mutex);
        writeLocked(kind, origin, address + offset, count, version, 0, 0,
                    snapshot);
        offset += count;
    }
}

void GuestEvents::channel(GMmu& space, GuestEventOrigin origin,
                          bool switch_channel) {
    if (!enabled())
        return;
    const auto id = space.getCaptureId();
    const auto generation = space.getMappingGeneration();
    // A mapping snapshot is taken only after its generation changes. It
    // includes sparse backing and physical host addresses so aliases retain
    // one backing identity instead of becoming independent resources.
    std::vector<std::pair<uptr, AddressSpace>> snapshot;
    {
        std::scoped_lock lock(mutex);
        if (spaces[id].generation == generation) {
            if (switch_channel)
                writeLocked("channel_switch", origin, id, 0, 0, 0, 0);
            return;
        }
    }
    snapshot = space.snapshotMappings();
    std::map<std::uintptr_t, Mapping> next;
    std::vector<Mapping> initial;
    for (const auto& [base, mapping] : snapshot)
        next.emplace(base, Mapping{mapping.ptr, mapping.size, mapping.guest});
    {
        std::scoped_lock lock(mutex);
        auto& previous = spaces[id];
        for (const auto& [base, mapping] : previous.mappings)
            if (!next.contains(base) || next.at(base) != mapping)
                writeLocked("unmap", origin, base, mapping.size, id, 0, 0);
        for (const auto& [base, mapping] : next)
            if (!previous.mappings.contains(base) ||
                previous.mappings.at(base) != mapping) {
                writeLocked("map", origin, base, mapping.size, id, mapping.host,
                            mapping.guest);
                initial.push_back(mapping);
            }
        previous = {generation, next};
        if (switch_channel)
            writeLocked("channel_switch", origin, id, 0, 0, 0, 0);
    }
    // Initial bytes are an observation, not a new CPU version. Replay applies
    // them only where it has no earlier memory contents/ownership evidence.
    for (const auto& mapping : initial) {
        if (mapping.host != 0)
            memory("memory_initial", mapping.host, mapping.size, 0, origin);
    }
}

void GuestEvents::initialSyncpoint(std::uint32_t id, std::uint32_t value) {
    if (!enabled())
        return;
    std::scoped_lock lock(mutex);
    if (initial_syncpoints.insert(id).second)
        writeLocked("syncpoint_initial", {}, id, 0, 0, value, 0);
}

void GuestEvents::finish() {
    std::scoped_lock lock(mutex);
    if (!enabled())
        return;
    writeLocked("session_end", {}, 0, pending_writes.size(),
                pending_native.size(),
                pending_writes.empty() && pending_reports.empty() &&
                        pending_native.empty()
                    ? 1
                    : 0,
                pending_reports.size());
    active.store(false, std::memory_order_release);
    output.close();
}

void GuestEvents::addressSpaceDestroyed(std::uint64_t id) {
    if (!enabled())
        return;
    std::scoped_lock lock(mutex);
    const auto found = spaces.find(id);
    if (found != spaces.end()) {
        for (const auto& [base, mapping] : found->second.mappings)
            writeLocked("unmap", {}, base, mapping.size, id, 0, 0);
        spaces.erase(found);
    }
    writeLocked("address_space_destroy", {}, id, 0, 0, 0, 0);
}

std::uint64_t GuestEvents::beginReport(GuestEventOrigin origin) {
    if (!enabled())
        return 0;
    std::scoped_lock lock(mutex);
    if (!enabled())
        return 0;
    const auto id = next_report++;
    pending_reports.insert(id);
    writeLocked("report_queued", origin, 0, 0, id, 0, 0);
    return id;
}

void GuestEvents::completeReport(std::uint64_t id, bool succeeded) {
    if (id == 0 || !enabled())
        return;
    std::scoped_lock lock(mutex);
    pending_reports.erase(id);
    writeLocked("report_complete", {}, 0, 0, id, succeeded ? 1 : 0, 0);
}

void recordGuestCommand(std::string_view kind, GuestEventOrigin origin,
                        const resource::ContentManifest& contents) {
    auto& events = GuestEvents::instance();
    if (!events.enabled())
        return;
    events.event(kind, origin);
    for (const auto& read : contents.reads)
        events.event("resource_read", origin, read.range.getBegin(),
                     read.range.getSize(), read.version);
    for (const auto& range : contents.writes)
        events.event("resource_write", origin, range.getBegin(),
                     range.getSize());
}

} // namespace hydra::hw::tegra_x1::gpu::capture

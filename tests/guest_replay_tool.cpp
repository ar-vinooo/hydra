// Offline replay through Hydra's production PFIFO decoder and GPU engines.
// Input is exported by tools/replay_guest_events.py --native-output.
// Observed word/channel order is retained; conservative checkpoints make this
// a correctness reference, not a performance measurement of the live scheduler.
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "guest_replay_format.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::guest_replay;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

u64 endOf(u64 begin, u64 size) {
    require(size <= std::numeric_limits<u64>::max() - begin,
            "captured address range overflows");
    return begin + size;
}

class Backing {
    struct Block {
        u64 begin, end;
        void* data;
    };
    std::vector<Block> blocks;
    // Ranges already initialized or produced by native work. A later initial
    // snapshot must not overwrite these bytes with capture-side observations.
    std::map<u64, u64> observed;

  public:
    ~Backing() {
        for (const auto& block : blocks)
            munmap(block.data, block.end - block.begin);
    }

    void allocate(const std::vector<Event>& events, u64 budget) {
        const long native_page = sysconf(_SC_PAGESIZE);
        require(native_page > 0, "cannot determine native page size");
        const auto page = static_cast<u64>(native_page);
        std::map<u64, u64> intervals;
        const auto add = [&](u64 begin, u64 size) {
            if (!size || !begin)
                return;
            const auto end = endOf(begin, size);
            require(end <= std::numeric_limits<u64>::max() - (page - 1),
                    "aligned backing range overflows");
            begin -= begin % page;
            const auto rounded = ((end + page - 1) / page) * page;
            intervals[begin] = std::max(intervals[begin], rounded);
        };
        for (const auto& event : events) {
            if (event.kind == Kind::Map)
                add(event.value, event.size);
            else if (event.kind == Kind::MemoryInitial ||
                     event.kind == Kind::MemoryWrite ||
                     event.kind == Kind::MemoryPublished ||
                     event.kind == Kind::MemoryWriteback)
                add(event.address, event.size);
        }
        std::vector<std::pair<u64, u64>> merged;
        for (const auto& [begin, end] : intervals) {
            if (!merged.empty() && begin <= merged.back().second)
                merged.back().second = std::max(merged.back().second, end);
            else
                merged.emplace_back(begin, end);
        }
        u64 allocated = 0;
        for (const auto& [begin, end] : merged) {
            require(end - begin <= budget - std::min(budget, allocated),
                    "native backing memory budget exceeded");
            auto* data = mmap(nullptr, end - begin, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
            require(data != MAP_FAILED, "cannot allocate native guest backing");
            blocks.push_back({begin, end, data});
            allocated += end - begin;
        }
    }

    u8* pointer(u64 address, u64 size) const {
        const auto end = endOf(address, size);
        for (const auto& block : blocks)
            if (block.begin <= address && end <= block.end)
                return static_cast<u8*>(block.data) + address - block.begin;
        throw std::runtime_error("event refers to uncaptured physical backing");
    }

    void mark(u64 begin, u64 size) {
        auto end = endOf(begin, size);
        if (begin == end)
            return;
        auto it = observed.lower_bound(begin);
        if (it != observed.begin() && std::prev(it)->second >= begin)
            --it;
        while (it != observed.end() && it->first <= end) {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            it = observed.erase(it);
        }
        observed.emplace(begin, end);
    }

    bool known(u64 begin, u64 size) const {
        auto it = observed.upper_bound(begin);
        return it != observed.begin() &&
               std::prev(it)->second >= endOf(begin, size);
    }

    void initialize(const Event& event) {
        require(event.size == event.data.size() && event.size != 0,
                "invalid initial memory payload");
        const auto end = endOf(event.address, event.size);
        u64 cursor = event.address;
        for (const auto& [begin, covered_end] : observed) {
            if (covered_end <= cursor || begin >= end)
                continue;
            if (cursor < begin)
                std::memcpy(pointer(cursor, begin - cursor),
                            event.data.data() + cursor - event.address,
                            begin - cursor);
            cursor = std::min(end, std::max(cursor, covered_end));
        }
        if (cursor < end)
            std::memcpy(pointer(cursor, end - cursor),
                        event.data.data() + cursor - event.address,
                        end - cursor);
        mark(event.address, event.size);
    }
};

struct Decoded {
    u64 channel, sequence, source;
    u32 method, argument, subchannel;
};
struct Observation {
    u64 order, address, size, hash;
    bool matches;
};

class Replay {
    Backing backing; // Must outlive Gpu, caches and retained mappings.
    Gpu gpu;
    std::map<u64, std::shared_ptr<GMmu>> spaces;
    std::map<u64, std::unique_ptr<PfifoDecodeState>> channels;
    std::map<u64, u64> bindings;
    std::set<u64> closed_channels, destroyed_spaces, initialized_syncpoints;
    std::map<u64, Event> waits;
    std::deque<Decoded> decoded;
    std::unique_ptr<renderer::ICommandBuffer> commands;
    const Event* pending_word{nullptr};
    bool segment_ended{false};
    std::mt19937_64 delays;
    u64 delay_limit, current_order{0}, methods{0}, words{0};
    std::vector<Observation> observations;

    PfifoDecodeState& state(u64 channel) {
        const auto found = channels.find(channel);
        require(found != channels.end() && !closed_channels.contains(channel),
                "event has no live channel");
        return *found->second;
    }

    void select(u64 channel) {
        auto& selected = state(channel);
        tls_crnt_decode_state = &selected;
        tls_crnt_gmmu = spaces.at(bindings.at(channel)).get();
        tls_crnt_command_buffer = commands.get();
    }

    void checkpoint() {
        require(gpu.getRenderer().synchronize(commands.get()),
                "native GPU submission failed");
        gpu.getRenderer().drainReports();
        gpu.getRenderer().applyQueuedMemoryInvalidations();
    }

    void flushWord() {
        if (!pending_word)
            return;
        const auto& event = *pending_word;
        pending_word = nullptr;
        require(!segment_ended && !waits.contains(event.channel),
                "word overtakes an ended segment or blocked channel");
        select(event.channel);
        auto& channel = state(event.channel);
        require(event.value <= UINT32_MAX && event.size == 4 &&
                    event.auxiliary == channel.remaining,
                "raw word or decoder packet state differs from capture");
        if (delay_limit)
            std::this_thread::sleep_for(
                std::chrono::microseconds(delays() % (delay_limit + 1)));
        segment_ended = !gpu.getPfifo().replayWord(
            static_cast<u32>(event.value), event.address, channel);
        require(!channel.failed, "native decoder or engine faulted");
        // Submission results are never supplied from captured GPU bytes.
        checkpoint();
        ++words;
    }

    void observe(const Event& event) {
        require(event.size == event.data.size() && event.size != 0,
                "invalid observed memory payload");
        checkpoint();
        auto* data = backing.pointer(event.address, event.size);
        const auto range = ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(data), event.size);
        gpu.getRenderer().getTextureCache().flushMemory(commands.get(), range);
        checkpoint();
        u64 hash = 14695981039346656037ull;
        for (u64 byte = 0; byte < event.size; ++byte) {
            hash ^= data[byte];
            hash *= 1099511628211ull;
        }
        const bool matches =
            std::memcmp(data, event.data.data(), event.size) == 0;
        observations.push_back(
            {event.order, event.address, event.size, hash, matches});
        backing.mark(event.address, event.size);
    }

  public:
    Replay(const std::vector<Event>& events, u64 budget, u64 seed,
           u64 max_delay)
        : delays(seed), delay_limit(seed ? max_delay : 0) {
        require(CONFIG_INSTANCE.getGpuRenderer() == GpuRenderer::Metal,
                "native rendering replay requires the Metal renderer");
        backing.allocate(events, budget);
        commands.reset(gpu.getRenderer().createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
    }

    ~Replay() {
        // Pending native submissions must release pointers before backing dies,
        // including on an exception during comparison.
        if (commands) {
            gpu.getRenderer().synchronize(commands.get());
            gpu.getRenderer().drainReports();
            commands.reset();
        }
        tls_crnt_command_buffer = nullptr;
        tls_crnt_decode_state = nullptr;
        tls_crnt_gmmu = nullptr;
    }

    void apply(const Event& event) {
        current_order = event.order;
        endOf(event.address, event.size);
        switch (event.kind) {
        case Kind::SessionBegin:
            require(event.order == 1, "duplicate session header");
            break;
        case Kind::SessionEnd:
            flushWord();
            checkpoint();
            require(decoded.empty() && waits.empty(),
                    "capture ended with unmatched methods or waits");
            for (const auto& [id, channel] : channels)
                require(
                    !channel->failed && !channel->semaphore_wait &&
                        !channel->syncpoint_wait && channel->remaining == 0 &&
                        channel->macro_args.empty(),
                    "capture ended with an unfinished native channel packet");
            break;
        case Kind::Map: {
            require(event.version &&
                        !destroyed_spaces.contains(event.version) && event.size,
                    "invalid captured address space mapping");
            auto& space = spaces[event.version];
            if (!space)
                space = std::make_shared<GMmu>(nullptr);
            const auto ptr = event.value
                                 ? reinterpret_cast<uptr>(
                                       backing.pointer(event.value, event.size))
                                 : 0;
            uptr base = 0;
            require(!space->findAddr(event.address, base),
                    "mapping replaces a live range without unmap");
            space->map(event.address, {ptr, event.size, event.auxiliary});
            break;
        }
        case Kind::Unmap: {
            checkpoint();
            auto& space = spaces.at(event.version);
            uptr base = 0;
            const auto mapping = space->findAddr(event.address, base);
            require(mapping && base == event.address &&
                        mapping->size == event.size,
                    "native unmap does not match capture");
            space->unmap(event.address);
            break;
        }
        case Kind::ChannelSwitch: {
            flushWord();
            checkpoint();
            require(event.channel && event.address &&
                        !closed_channels.contains(event.channel) &&
                        !destroyed_spaces.contains(event.address),
                    "invalid channel switch");
            auto& space = spaces[event.address];
            if (!space)
                space = std::make_shared<GMmu>(nullptr);
            auto& channel = channels[event.channel];
            if (!channel) {
                channel = std::make_unique<PfifoDecodeState>();
                channel->context = std::make_unique<GpuChannelContext>(gpu);
                channel->address_space = space;
                const auto id = event.channel;
                channel->replay_method_observer =
                    [this, id](u32 method, u32 argument, u64 sequence,
                               uptr source, u32 subchannel) {
                        decoded.push_back({id, sequence, source, method,
                                           argument, subchannel});
                    };
                bindings.emplace(event.channel, event.address);
            }
            require(bindings.at(event.channel) == event.address,
                    "channel changed address space");
            select(event.channel);
            break;
        }
        case Kind::ChannelDestroy:
            flushWord();
            checkpoint();
            require(!closed_channels.contains(event.channel),
                    "channel destroyed twice");
            closed_channels.insert(event.channel);
            require(!waits.contains(event.channel),
                    "channel destroyed while waiting");
            break;
        case Kind::AddressSpaceDestroy:
            flushWord();
            checkpoint();
            require(!destroyed_spaces.contains(event.address),
                    "address space destroyed twice");
            if (spaces.contains(event.address))
                require(spaces.at(event.address)->snapshotMappings().empty(),
                        "destroyed address space retains mappings");
            destroyed_spaces.insert(event.address);
            break;
        case Kind::PfifoEntry:
            flushWord();
            segment_ended = false;
            select(event.channel);
            break;
        case Kind::PfifoWord:
            flushWord();
            pending_word = &event;
            break;
        case Kind::Method: {
            // Mappings and initial bytes recorded between word fetch and method
            // dispatch have now been restored. This also handles macro packets
            // whose final argument dispatches several accumulated methods.
            flushWord();
            require(!decoded.empty(), "capture method has no decoded raw word");
            const auto actual = decoded.front();
            decoded.pop_front();
            require(actual.channel == event.channel &&
                        actual.sequence == event.sequence &&
                        actual.source == event.source &&
                        actual.method == event.address &&
                        actual.argument == event.value &&
                        actual.subchannel == event.auxiliary,
                    "native method trace differs from captured PFIFO decode");
            ++methods;
            break;
        }
        case Kind::MemoryInitial:
            backing.initialize(event);
            break;
        case Kind::MemoryWrite:
            if (event.channel) {
                // DMA, inline upload and synchronous releases already ran in
                // the engine. Compare them instead of injecting expected
                // output.
                observe(event);
            } else {
                require(event.size == event.data.size() && event.size,
                        "invalid CPU input memory payload");
                checkpoint();
                auto* ptr = backing.pointer(event.address, event.size);
                const auto range = ztd::Range<uptr>::fromSize(
                    reinterpret_cast<uptr>(ptr), event.size);
                gpu.getRenderer().getTextureCache().discardGpuWrites(
                    commands.get(), range);
                std::memcpy(ptr, event.data.data(), event.size);
                gpu.getRenderer().invalidateMemory(range);
                backing.mark(event.address, event.size);
            }
            break;
        case Kind::MemoryPublished:
        case Kind::MemoryWriteback:
            observe(event);
            break;
        case Kind::GpuWriteIssued:
            backing.mark(event.address, event.size);
            break;
        case Kind::ResourceRead:
            require(backing.known(event.address, event.size),
                    "native read lacks captured initial bytes or producer");
            break;
        case Kind::SemaphoreWait:
        case Kind::SyncpointWait:
            require(!waits.contains(event.channel),
                    "channel has duplicate wait");
            waits.emplace(event.channel, event);
            break;
        case Kind::SemaphoreAcquired:
        case Kind::SemaphoreResume: {
            checkpoint();
            select(event.channel);
            auto& channel = state(event.channel);
            auto* word = resolveSemaphoreWord(*tls_crnt_gmmu, event.address);
            require(word && event.version <= UINT32_MAX &&
                        event.value <= UINT32_MAX,
                    "invalid replay semaphore");
            const u32 actual =
                std::atomic_ref<u32>(*word).load(std::memory_order_acquire);
            const auto expected = static_cast<u32>(event.version);
            const bool satisfied =
                event.auxiliary == 1 ? actual == expected
                : event.auxiliary == 4
                    ? static_cast<i32>(actual - expected) >= 0
                    : event.auxiliary == 8 && (actual & expected) != 0;
            require(satisfied && actual == event.value,
                    "native semaphore value differs from capture");
            if (event.kind == Kind::SemaphoreResume) {
                const auto& wait = waits.at(event.channel);
                require(wait.kind == Kind::SemaphoreWait &&
                            wait.sequence == event.sequence &&
                            wait.address == event.address &&
                            wait.value == event.version &&
                            wait.auxiliary == event.auxiliary,
                        "semaphore resume has no matching wait");
                waits.erase(event.channel);
            }
            channel.semaphore_wait.reset();
            break;
        }
        case Kind::SyncpointInitial: {
            require(event.address > 0 && event.address < 192 &&
                        event.value <= UINT32_MAX &&
                        initialized_syncpoints.insert(event.address).second,
                    "invalid initial syncpoint");
            auto& syncpoints = gpu.getVideoSyncpoints();
            while (!syncpoints.read(static_cast<u32>(event.address)))
                require(syncpoints.allocate().has_value(),
                        "cannot restore syncpoint ID");
            require(syncpoints.read(static_cast<u32>(event.address)).value() ==
                        0,
                    "syncpoint was used before its initial state");
            syncpoints.complete(static_cast<u32>(event.address),
                                static_cast<u32>(event.value));
            break;
        }
        case Kind::SyncpointResume: {
            checkpoint();
            const auto& wait = waits.at(event.channel);
            const auto actual =
                gpu.getVideoSyncpoints().read(static_cast<u32>(event.address));
            require(wait.kind == Kind::SyncpointWait &&
                        wait.sequence == event.sequence &&
                        wait.address == event.address &&
                        wait.value == event.version && actual &&
                        event.version <= UINT32_MAX && event.value == *actual &&
                        video::Syncpoints::reached(
                            *actual, static_cast<u32>(event.version)),
                    "native syncpoint dependency was not reproduced");
            state(event.channel).syncpoint_wait.reset();
            waits.erase(event.channel);
            break;
        }
        case Kind::ChannelFault:
            throw std::runtime_error(
                "faulted captures cannot be native replay references");
        case Kind::GpuComplete:
        case Kind::ReportComplete:
        case Kind::NativeComplete:
            require(event.value == 1,
                    "capture contains failed GPU work or report");
            break;
        default:
            // Operation/access/epoch/report markers describe methods executed
            // by the raw decoder. They never execute a second operation.
            break;
        }
    }

    bool writeSummary(std::ostream& output) const {
        const auto mismatches =
            std::count_if(observations.begin(), observations.end(),
                          [](const Observation& observation) {
                              return !observation.matches;
                          });
        output
            << "{\"schema\":1,\"status\":\""
            << (mismatches ? "mismatch" : "replayed")
            << "\",\"rendered\":true,\"decoder\":\"pfifo\",\"guest_methods\":"
            << methods << ",\"pfifo_words\":" << words
            << ",\"events\":" << current_order
            << ",\"mismatches\":" << mismatches << ",\"observations\":[";
        bool first = true;
        for (const auto& item : observations) {
            if (!first)
                output << ',';
            first = false;
            output << "{\"order\":" << item.order
                   << ",\"address\":" << item.address
                   << ",\"size\":" << item.size << ",\"fnv1a64\":" << item.hash
                   << ",\"matches\":" << (item.matches ? "true" : "false")
                   << '}';
        }
        output << "]}\n";
        require(static_cast<bool>(output),
                "cannot write native replay summary");
        return mismatches == 0 && methods != 0 && words != 0;
    }
};

u64 number(const char* text) {
    const std::string value(text);
    require(!value.empty() && value.front() != '-',
            "expected an unsigned option value");
    size_t consumed = 0;
    const auto result = std::stoull(value, &consumed);
    require(consumed == value.size(), "invalid numeric option");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr
            << "usage: hydra-guest-replay <events.hge> [--output summary.json] "
               "[--budget-mb 512] [--delay-seed N] [--max-delay-us 500]\n";
        return 2;
    }
    try {
        std::string destination;
        u64 budget_mb = 512, seed = 0, max_delay = 500;
        for (int index = 2; index < argc; index += 2) {
            require(index + 1 < argc, "option is missing its value");
            const std::string option(argv[index]);
            if (option == "--output")
                destination = argv[index + 1];
            else if (option == "--budget-mb")
                budget_mb = number(argv[index + 1]);
            else if (option == "--delay-seed")
                seed = number(argv[index + 1]);
            else if (option == "--max-delay-us")
                max_delay = number(argv[index + 1]);
            else
                throw std::runtime_error("unknown replay option");
        }
        require(budget_mb > 0 && budget_mb <= 16384 && max_delay <= 1000000,
                "replay budget or delay exceeds its supported limit");
        if (!destination.empty())
            require(std::filesystem::absolute(destination).lexically_normal() !=
                        std::filesystem::absolute(argv[1]).lexically_normal(),
                    "summary output must differ from the capture");
        unsetenv("HYDRA_GUEST_EVENTS");
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        std::ifstream input(argv[1], std::ios::binary);
        require(static_cast<bool>(input), "cannot open native guest capture");
        const auto events = readEvents(input, budget_mb << 20);
        Replay replay(events, budget_mb << 20, seed, max_delay);
        for (const auto& event : events) {
            try {
                replay.apply(event);
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "event " + std::to_string(event.order) + ", channel " +
                    std::to_string(event.channel) + ": " + error.what());
            }
        }
        if (destination.empty())
            return replay.writeSummary(std::cout) ? 0 : 1;
        std::ofstream output(destination);
        require(static_cast<bool>(output), "cannot open replay summary output");
        return replay.writeSummary(output) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

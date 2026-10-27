#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"

#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::resource;
using R = ztd::Range<uptr>;

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

ContentVersions::GpuWrite issue(ContentVersions& versions, R range) {
    return versions.issueGpuWrites(std::array{range});
}

void testCompletions() {
    ContentVersions versions;
    auto old = issue(versions, R(0x1000, 0x2000));
    auto state = versions.inspect(R(0x1800, 0x2800));
    check(state.issued == old.version() && state.completed == 0 &&
              state.pending && !state.failed,
          "Encoded GPU writes are pending across partial aliases");
    check(!versions.inspect(R(0x2000, 0x3000)).pending,
          "Adjacent intervals do not alias");
    const auto cpu = versions.write(R(0x1800, 0x1900));
    old.complete(true);
    state = versions.inspect(R(0x1800, 0x1900));
    check(state.issued == cpu && state.completed == cpu && !state.pending,
          "Late GPU completion cannot reclaim newer CPU bytes");
    state = versions.inspect(R(0x1900, 0x2000));
    check(state.issued == old.version() && state.completed == old.version() &&
              !state.pending,
          "Split tails retain their original completion status");
    auto failed = issue(versions, R(0x1800, 0x1900));
    failed.complete(false);
    failed.complete(true);
    state = versions.inspect(R(0x1800, 0x1900));
    check(state.failed && !state.pending && state.completed == 0,
          "Failure cannot become successful through a duplicate callback");
    auto first = issue(versions, R(0x4000, 0x5000));
    auto second = issue(versions, R(0x4000, 0x5000));
    second.complete(true);
    first.complete(false);
    state = versions.inspect(R(0x4000, 0x5000));
    check(state.issued == second.version() && state.completed == second.version() &&
              !state.failed,
          "Out-of-order completion cannot replace a newer GPU owner");
    const auto before = versions.current();
    check(versions.issueGpuWrites({}).version() == 0 &&
              issue(versions, R(3, 3)).version() == 0 && versions.current() == before,
          "Empty GPU writes do not allocate epochs");
    ContentVersions::GpuWrite orphan;
    {
        ContentVersions temporary;
        orphan = issue(temporary, R(1, 2));
    }
    orphan.complete(true);
    ContentVersions bounded;
    auto evicted = issue(bounded, R(0, 1));
    for (uptr i = 1; i <= ContentVersions::MAX_RANGES; ++i)
        bounded.write(R(i * 2, i * 2 + 1));
    evicted.complete(true);
    state = bounded.inspect(R(0, 1));
    check(bounded.size() <= ContentVersions::MAX_RANGES && state.unknown &&
              state.completed == 0,
          "Eviction cannot invent proof of completion");
}

void testDifferential() {
    // Independent byte-array oracle: each byte has its latest issued owner,
    // and completion updates only the owner's status, never the byte's owner.
    constexpr uptr BYTES = 256;
    for (unsigned seed = 0; seed < 32; ++seed) {
        ContentVersions versions;
        std::array<u64, BYTES> owners{};
        std::vector<int> status{1}; // 0 pending, 1 completed, 2 failed
        std::vector<ContentVersions::GpuWrite> tokens(1);
        std::mt19937 random(seed);
        for (unsigned step = 0; step < 4000; ++step) {
            const auto operation = random() % 4;
            const uptr begin = random() % BYTES;
            const uptr end = begin + 1 + random() % (BYTES - begin);
            if (operation <= 1) {
                const bool gpu = operation == 0;
                auto token = gpu ? issue(versions, R(begin, end))
                                 : ContentVersions::GpuWrite{};
                const auto version = gpu ? token.version() : versions.write(R(begin, end));
                check(version == status.size(), "Every issue gets one ordered epoch");
                status.push_back(gpu ? 0 : 1);
                tokens.push_back(token);
                for (auto i = begin; i < end; ++i)
                    owners[i] = version;
            } else if (status.size() > 1) {
                const auto id = 1 + random() % (status.size() - 1);
                const bool success = random() % 2 != 0;
                tokens[id].complete(success);
                if (status[id] == 0)
                    status[id] = success ? 1 : 2;
            }
            ContentVersions::State expected;
            for (auto i = begin; i < end; ++i) {
                const auto owner = owners[i];
                expected.issued = std::max(expected.issued, owner);
                if (status[owner] == 1)
                    expected.completed = std::max(expected.completed, owner);
                expected.pending |= status[owner] == 0;
                expected.failed |= status[owner] == 2;
            }
            const auto actual = versions.inspect(R(begin, end));
            check(actual.issued == expected.issued &&
                      actual.completed == expected.completed &&
                      actual.pending == expected.pending &&
                      actual.failed == expected.failed && !actual.unknown &&
                      versions.latest(R(begin, end)) == expected.issued,
                  "Interval ownership and completion match byte-array oracle");
        }
    }
}

void testConcurrency() {
    ContentVersions versions;
    std::array<std::thread, 4> workers;
    std::array<u64, 4> last{};
    for (uptr t = 0; t < workers.size(); ++t)
        workers[t] = std::thread([&, t] {
            for (unsigned i = 0; i < 2000; ++i) {
                auto token = issue(versions, R(t * 4096, t * 4096 + 2048));
                token.complete(true);
                last[t] = token.version();
            }
        });
    for (auto& worker : workers)
        worker.join();
    check(versions.current() == 8000, "Concurrent GPU issues retain every epoch");
    for (uptr t = 0; t < workers.size(); ++t) {
        const auto state = versions.inspect(R(t * 4096, t * 4096 + 2048));
        check(state.issued == last[t] && state.completed == last[t] && !state.pending,
              "Concurrent completion keeps the latest owner");
    }
}
} // namespace

int main() {
    try {
        testCompletions();
        testDifferential();
        testConcurrency();
        std::cout << "PASS: completion races, alias ranges, 128000 differential steps, "
                     "8000 concurrent GPU writes\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

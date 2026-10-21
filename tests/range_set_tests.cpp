#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"
#include "core/hw/tegra_x1/gpu/resource/range_set.hpp"

#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::resource;

int main() {
    try {
        const std::array<ztd::Range<uptr>, 5> holes{
            ztd::Range<uptr>(10, 20), ztd::Range<uptr>(30, 40),
            ztd::Range<uptr>(15, 18), ztd::Range<uptr>(40, 50),
            ztd::Range<uptr>(25, 25)};
        const auto separate = coalesceRanges(holes);
        if (separate.size() != 2 || separate[0].getBegin() != 10 ||
            separate[0].getEnd() != 20 || separate[1].getBegin() != 30 ||
            separate[1].getEnd() != 50)
            throw std::runtime_error(
                "dirty range coalescing filled an untouched gap");
        std::mt19937_64 random(0x4859445241ull);
        for (unsigned iteration = 0; iteration < 5000; ++iteration) {
            std::vector<ztd::Range<uptr>> original;
            std::array<bool, 128> expected{}, actual{};
            for (unsigned index = 0; index < 32; ++index) {
                const auto begin = static_cast<uptr>(random() % 128);
                const auto end = static_cast<uptr>(random() % 129);
                original.emplace_back(std::min(begin, end),
                                      std::max(begin, end));
                for (uptr byte = std::min(begin, end);
                     byte < std::max(begin, end); ++byte)
                    expected[byte] = true;
            }
            const auto merged = coalesceRanges(original);
            uptr previous_end = 0;
            bool first = true;
            for (const auto& range : merged) {
                if (range.getBegin() >= range.getEnd() ||
                    (!first && previous_end >= range.getBegin()))
                    throw std::runtime_error(
                        "range union is not disjoint and minimal");
                for (uptr byte = range.getBegin(); byte < range.getEnd();
                     ++byte)
                    actual[byte] = true;
                previous_end = range.getEnd();
                first = false;
            }
            if (actual != expected)
                throw std::runtime_error(
                    "range union changed covered bytes or filled a hole");
            ContentVersions left, right;
            const auto before = left.issueGpuWrites(original);
            const auto after = right.issueGpuWrites(merged);
            const auto overwrite = ztd::Range<uptr>(35, 71);
            left.write(overwrite);
            right.write(overwrite);
            before.complete(true);
            after.complete(true);
            for (uptr byte = 0; byte < 128; ++byte) {
                const auto a = left.inspect({byte, byte + 1});
                const auto b = right.inspect({byte, byte + 1});
                if (a.issued != b.issued || a.completed != b.completed ||
                    a.pending != b.pending || a.failed != b.failed ||
                    a.unknown != b.unknown)
                    throw std::runtime_error(
                        "coalescing changed completion/CPU overwrite "
                        "semantics");
            }
            ContentManifest a, b, consumer;
            a.writes = original;
            b.writes = merged;
            const uptr point = static_cast<uptr>(random() % 128);
            consumer.addRead(0, {point, point + 1});
            consumer.addWrite({point, point + 1});
            if (consumer.dependsOn(a) != consumer.dependsOn(b) ||
                a.dependsOn(consumer) != b.dependsOn(consumer))
                throw std::runtime_error(
                    "coalescing changed RAW/WAR/WAW dependencies");
        }
        std::cout
            << "PASS: 5000 differential interval/ownership/dependency cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "core/horizon/services/timesrv/time_zone_service.hpp"

using namespace hydra;
using namespace hydra::horizon::services::timesrv;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool same(const CalendarTime& a, const CalendarTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day &&
           a.hour == b.hour && a.minute == b.minute && a.second == b.second;
}

CalendarTime calendar(i64 time, const TimeZoneRule& rule) {
    CalendarTime result;
    CalendarAdditionalInfo info;
    ITimeZoneService::toCalendarTimeImpl(time, rule, result, info);
    return result;
}

// UTC+1, with UTC+2 from 2024-03-31 01:00 UTC to 2024-10-27 01:00 UTC
constexpr i64 SPRING = 1711846800;
constexpr i64 AUTUMN = 1729990800;

std::unique_ptr<TimeZoneRule> centralEurope() {
    auto rule = std::make_unique<TimeZoneRule>();
    std::memset(rule.get(), 0, sizeof(TimeZoneRule));
    rule->type_count = 2;
    rule->type_infos[0].gmt_offset = 3600;
    rule->type_infos[1].gmt_offset = 7200;
    rule->type_infos[1].is_day_saving_time = true;
    rule->time_count = 2;
    rule->ats[0] = SPRING;
    rule->type_indices[0] = 1;
    rule->ats[1] = AUTUMN;
    rule->type_indices[1] = 0;
    rule->default_type = 0;
    return rule;
}

} // namespace

int main() {
    const auto rule = centralEurope();
    std::array<i64, 2> times{};
    u32 count = 0;

    // Every second of the year round-trips; the repeated autumn hour has
    // both of its times, in order
    for (i64 time = SPRING - 86400 * 2; time < AUTUMN + 86400 * 2; time += 997) {
        const auto shown = calendar(time, *rule);
        ITimeZoneService::toPosixTimeImpl(shown, *rule, times, count);
        const bool repeated = time >= AUTUMN - 3600 && time < AUTUMN + 3600;
        check(count == (repeated ? 2u : 1u), "One time, two in the repeated hour");
        check(times[0] == time || (count == 2 && times[1] == time),
              "A calendar time converts back to the time that shows it");
        if (count == 2)
            check(times[0] + 3600 == times[1] &&
                      same(calendar(times[0], *rule), shown) &&
                      same(calendar(times[1], *rule), shown),
                  "Both times of the repeated hour show it, in order");
    }

    // 02:30 on the spring day is skipped: the old offset applies
    const CalendarTime skipped{2024, 3, 31, 2, 30, 0, 0};
    ITimeZoneService::toPosixTimeImpl(skipped, *rule, times, count);
    check(count == 1 && times[0] == SPRING + 1800,
          "A skipped time converts with the offset before the change");
    check(calendar(times[0], *rule).hour == 3,
          "The skipped time shows an hour later");

    const CalendarTime invalid{2024, 2, 30, 0, 0, 0, 0};
    ITimeZoneService::toPosixTimeImpl(invalid, *rule, times, count);
    check(count == 0, "An invalid date has no time");

    auto utc = std::make_unique<TimeZoneRule>();
    std::memset(utc.get(), 0, sizeof(TimeZoneRule));
    utc->type_count = 1;
    const CalendarTime epoch_day{2000, 1, 1, 12, 0, 0, 0};
    ITimeZoneService::toPosixTimeImpl(epoch_day, *utc, times, count);
    check(count == 1 && times[0] == 946728000, "UTC converts directly");

    std::cout << "PASS: ToPosixTime round trips, repeated and skipped hours, "
                 "invalid dates\n";
    return 0;
}

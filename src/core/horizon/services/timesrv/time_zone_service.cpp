#include "core/horizon/services/timesrv/time_zone_service.hpp"

#include "core/system.hpp"

#include <algorithm>
#include <vector>

namespace hydra::horizon::services::timesrv {

DEFINE_SERVICE_COMMAND_TABLE(ITimeZoneService, 0, getDeviceLocationName, 4,
                             loadTimeZoneRule, 100, toCalendarTime, 101,
                             toCalendarTimeWithMyRule, 201, toPosixTime, 202,
                             toPosixTimeWithMyRule)

result_t ITimeZoneService::getDeviceLocationName(RequestContext* ctx,
                                                 LocationName* out_name) {
    const auto name = ctx->system.getOs()
                          .getTimeManager()
                          .getTimeZoneManager()
                          .getDeviceLocationName();
    std::memcpy(out_name->name, name.data(), name.size());
    out_name->name[name.size()] = '\0';
    return RESULT_SUCCESS;
}

result_t ITimeZoneService::loadTimeZoneRule(
    RequestContext* ctx, LocationName location_name,
    OutBuffer<BufferAttr::MapAlias> out_rule_buffer) {
    TimeZoneRule rule;
    ctx->system.getOs().getTimeManager().getTimeZoneManager().loadRule(
        location_name.name, rule);

    out_rule_buffer.stream->write(rule);
    return RESULT_SUCCESS;
}

result_t
ITimeZoneService::toCalendarTime(i64 posix_time,
                                 InBuffer<BufferAttr::MapAlias> in_rule_buffer,
                                 ToCalendarTimeWithMyRuleOut* out) {
    return toCalendarTimeImpl(posix_time,
                              in_rule_buffer.stream->read<TimeZoneRule>(),
                              out->time, out->additional_info);
}

result_t
ITimeZoneService::toCalendarTimeWithMyRule(RequestContext* ctx, i64 posix_time,
                                           ToCalendarTimeWithMyRuleOut* out) {
    return toCalendarTimeImpl(
        posix_time,
        ctx->system.getOs().getTimeManager().getTimeZoneManager().getMyRule(),
        out->time, out->additional_info);
}

void ITimeZoneService::writePosixTimes(
    const std::array<i64, 2>& times, u32 count,
    OutBuffer<BufferAttr::HipcPointer>& out_buffer, i32* out_count) {
    // As many as the guest buffer holds
    const auto capacity = out_buffer.stream->getSize() / sizeof(i64);
    const auto written = std::min<u64>(count, capacity);
    for (u64 i = 0; i < written; i++)
        out_buffer.stream->write(times[i]);
    *out_count = static_cast<i32>(written);
}

result_t ITimeZoneService::toPosixTime(
    CalendarTime calendar_time, InBuffer<BufferAttr::MapAlias> in_rule_buffer,
    i32* out_count, OutBuffer<BufferAttr::HipcPointer> out_buffer) {
    std::array<i64, 2> times{};
    u32 count = 0;
    const auto res = toPosixTimeImpl(
        calendar_time, in_rule_buffer.stream->read<TimeZoneRule>(), times,
        count);
    writePosixTimes(times, count, out_buffer, out_count);
    return res;
}

result_t ITimeZoneService::toPosixTimeWithMyRule(
    RequestContext* ctx, CalendarTime calendar_time, i32* out_count,
    OutBuffer<BufferAttr::HipcPointer> out_buffer) {
    std::array<i64, 2> times{};
    u32 count = 0;
    const auto res = toPosixTimeImpl(
        calendar_time,
        ctx->system.getOs().getTimeManager().getTimeZoneManager().getMyRule(),
        times, count);
    writePosixTimes(times, count, out_buffer, out_count);
    return res;
}

result_t ITimeZoneService::toCalendarTimeImpl(
    i64 posix_time, const TimeZoneRule& rule, CalendarTime& out_time,
    CalendarAdditionalInfo& out_additional_info) {
    // Find the type
    u32 type_idx = rule.default_type;
    for (u32 i = 0; i < rule.time_count; ++i) {
        if (posix_time >= rule.ats[i]) {
            type_idx = rule.type_indices[i];
        } else {
            break;
        }
    }

    const auto& info = rule.type_infos[type_idx];

    // Adjust by GMT offset
    auto adjusted_tp = std::chrono::sys_seconds{
        std::chrono::seconds{posix_time + info.gmt_offset}};
    auto days = std::chrono::floor<std::chrono::days>(adjusted_tp);
    std::chrono::year_month_day ymd{days};
    std::chrono::hh_mm_ss hms{adjusted_tp - days};

    // Get time zone name
    const char* tz_name = rule.chars + info.abbreviation_list_index;

    // Output
    out_time = {
        .year = static_cast<u16>(static_cast<int>(ymd.year())),
        .month = static_cast<u8>(ymd.month().operator unsigned()),
        .day = static_cast<u8>(ymd.day().operator unsigned()),
        .hour = static_cast<u8>(hms.hours().count()),
        .minute = static_cast<u8>(hms.minutes().count()),
        .second = static_cast<u8>(hms.seconds().count()),
    };

    out_additional_info = {
        .day_of_week = static_cast<u8>(
            std::chrono::year_month_weekday{ymd}.weekday().c_encoding()),
        .day_of_year =
            static_cast<u32>((std::chrono::sys_days{ymd} -
                              std::chrono::sys_days{ymd.year() / 1 / 0})
                                 .count()),
        .timezone_name = stringAsU64(tz_name),
        .dst = info.is_day_saving_time ? 1u : 0u,
        .seconds_rel_to_utc = info.gmt_offset,
    };

    return RESULT_SUCCESS;
}

result_t ITimeZoneService::toPosixTimeImpl(const CalendarTime& calendar_time,
                                           const TimeZoneRule& rule,
                                           std::array<i64, 2>& out_times,
                                           u32& out_count) {
    out_count = 0;
    const std::chrono::year_month_day ymd{
        std::chrono::year{calendar_time.year},
        std::chrono::month{calendar_time.month},
        std::chrono::day{calendar_time.day}};
    if (!ymd.ok() || calendar_time.hour > 23 || calendar_time.minute > 59 ||
        calendar_time.second > 59)
        return RESULT_SUCCESS; // No time shows an invalid calendar time
    // The local time read as UTC
    const i64 local =
        std::chrono::sys_days{ymd}.time_since_epoch().count() * 86400 +
        calendar_time.hour * 3600 + calendar_time.minute * 60 +
        calendar_time.second;

    // A time shows `calendar_time` with the offset in effect at it
    const auto shows = [&](i64 time) {
        CalendarTime shown;
        CalendarAdditionalInfo info;
        toCalendarTimeImpl(time, rule, shown, info);
        return shown.year == calendar_time.year &&
               shown.month == calendar_time.month &&
               shown.day == calendar_time.day &&
               shown.hour == calendar_time.hour &&
               shown.minute == calendar_time.minute &&
               shown.second == calendar_time.second;
    };
    std::vector<i64> found;
    const u32 type_count = std::min<u32>(
        std::max<u32>(rule.type_count, 1), TimeZoneRule::MAX_TYPE_COUNT);
    for (u32 type = 0; type < type_count; type++) {
        const i64 time = local - rule.type_infos[type].gmt_offset;
        if (shows(time) &&
            std::find(found.begin(), found.end(), time) == found.end())
            found.push_back(time);
    }
    std::sort(found.begin(), found.end());

    if (found.empty()) {
        // Skipped by a change to a larger offset b from a: L - b is before
        // the change, so the offset there is a, and L - a shows a later time
        const auto offset_at = [&](i64 time) {
            u32 type_idx = rule.default_type;
            for (u32 i = 0; i < rule.time_count; ++i) {
                if (time >= rule.ats[i])
                    type_idx = rule.type_indices[i];
                else
                    break;
            }
            return i64(rule.type_infos[type_idx].gmt_offset);
        };
        i64 largest = rule.type_infos[0].gmt_offset;
        for (u32 type = 1; type < type_count; type++)
            largest = std::max<i64>(largest, rule.type_infos[type].gmt_offset);
        found.push_back(local - offset_at(local - largest));
    }
    out_count = static_cast<u32>(std::min<usize>(found.size(), out_times.size()));
    for (u32 i = 0; i < out_count; i++)
        out_times[i] = found[i];
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::timesrv

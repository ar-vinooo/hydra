#pragma once

#include <array>

#include "core/horizon/services/const.hpp"
#include "core/horizon/services/timesrv/const.hpp"

namespace hydra::horizon::services::timesrv {

struct LocationName {
    char name[0x24];
};

struct CalendarTime {
    u16 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u8 _unknown;
};

struct CalendarAdditionalInfo {
    u32 day_of_week; // 0-based
    u32 day_of_year; // 0-based
    u64 timezone_name;
    u32 dst; // TODO: what is this? (0 = no DST, 1 = DST)
    i32 seconds_rel_to_utc;
};

namespace {

struct ToCalendarTimeWithMyRuleOut {
    CalendarTime time;
    CalendarAdditionalInfo additional_info;
};

} // namespace

class ITimeZoneService : public IService {
  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // Commands
    result_t getDeviceLocationName(RequestContext* ctx, LocationName* out_name);
    result_t loadTimeZoneRule(RequestContext* ctx, LocationName location_name,
                              OutBuffer<BufferAttr::MapAlias> out_rule_buffer);
    result_t toCalendarTime(i64 posix_time,
                            InBuffer<BufferAttr::MapAlias> in_rule_buffer,
                            ToCalendarTimeWithMyRuleOut* out);
    result_t toCalendarTimeWithMyRule(RequestContext* ctx, i64 posix_time,
                                      ToCalendarTimeWithMyRuleOut* out);
    result_t toPosixTime(CalendarTime calendar_time,
                         InBuffer<BufferAttr::MapAlias> in_rule_buffer,
                         i32* out_count,
                         OutBuffer<BufferAttr::HipcPointer> out_buffer);
    result_t
    toPosixTimeWithMyRule(RequestContext* ctx, CalendarTime calendar_time,
                          i32* out_count,
                          OutBuffer<BufferAttr::HipcPointer> out_buffer);

  public:
    // Conversions, without service state
    static result_t
    toCalendarTimeImpl(i64 posix_time, const TimeZoneRule& rule,
                       CalendarTime& out_time,
                       CalendarAdditionalInfo& out_additional_info);
    // The POSIX times showing `calendar_time` locally: none or one, or two
    // when clocks are set back (in ascending order). A time skipped by a
    // forward change is taken with the offset before it, like mktime.
    static result_t toPosixTimeImpl(const CalendarTime& calendar_time,
                                    const TimeZoneRule& rule,
                                    std::array<i64, 2>& out_times,
                                    u32& out_count);

  private:
    static void writePosixTimes(const std::array<i64, 2>& times, u32 count,
                                OutBuffer<BufferAttr::HipcPointer>& out_buffer,
                                i32* out_count);
};

} // namespace hydra::horizon::services::timesrv

ENABLE_STRUCT_FORMATTING(hydra::horizon::services::timesrv::CalendarTime, year,
                         "", "year", month, "", "month", day, "", "day", hour,
                         "", "hour", minute, "", "minute", second, "", "second")

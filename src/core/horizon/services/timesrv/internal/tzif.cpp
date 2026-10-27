#include "core/horizon/services/timesrv/internal/tzif.hpp"

#include <bit>
#include <cstring>

namespace hydra::horizon::services::timesrv::internal {

namespace {

const int YEARS_PER_REPEAT = 400;
const long AVERAGE_SECONDS_PER_YEAR = 31556952;
const long SECONDS_PER_REPEAT = YEARS_PER_REPEAT * AVERAGE_SECONDS_PER_YEAR;

struct TzifHeader {
    u32 magic;
    u8 version;
    u8 _reserved[15];
    u32 ttis_gmt_count;
    u32 ttis_std_count;
    u32 leap_count;
    u32 time_count;
    u32 type_count;
    u32 char_count;
};

template <typename T>
T decode(T value) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    } else {
        return value;
    }
}

bool differByRepeat(i64 t1, i64 t0) { return (t1 - t0) == SECONDS_PER_REPEAT; }

bool timeTypeEquals(const TimeZoneRule& rule, u8 a_index, u8 b_index) {
    if (a_index < 0 || a_index >= rule.type_count || b_index < 0 ||
        b_index >= rule.type_count) {
        return false;
    }

    TimeTypeInfo a = rule.type_infos[a_index];
    TimeTypeInfo b = rule.type_infos[b_index];

    return a.gmt_offset == b.gmt_offset &&
           a.is_day_saving_time == b.is_day_saving_time &&
           a.is_standard_time_daylight == b.is_standard_time_daylight &&
           a.is_gmt == b.is_gmt &&
           std::string_view(rule.chars + a.abbreviation_list_index) ==
               std::string_view(rule.chars + b.abbreviation_list_index);
}


// POSIX TZ rule strings ("EST5EDT,M3.2.0,M11.1.0") in the footer of a TZif
// file. Port of tzparse() from the IANA tzcode (as in Nintendo's service and
// Ryujinx): the rule is expanded into explicit transitions from 1970 so that
// local time stays correct after the last transition stored in the file.
constexpr int SECONDS_PER_MINUTE = 60;
constexpr int SECONDS_PER_HOUR = 3600;
constexpr int SECONDS_PER_DAY = 86400;
constexpr int DAYS_PER_WEEK = 7;
constexpr int MONTHS_PER_YEAR = 12;
constexpr int DAYS_PER_NORMAL_YEAR = 365;
constexpr int DAYS_PER_LEAP_YEAR = 366;
constexpr int EPOCH_YEAR = 1970;

constexpr int MONTH_LENGTHS[2][MONTHS_PER_YEAR] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

bool isLeap(i64 year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

enum class PosixRuleType { JulianDay, DayOfYear, MonthNthDayOfWeek };

struct PosixRule {
    PosixRuleType type;
    i32 day;
    i32 week;
    i32 month;
    i32 time; // seconds after midnight
};

// All helpers return nullptr on malformed input.
const char* getZoneName(const char* str) {
    while (*str != '\0' && !isDigit(*str) && *str != ',' && *str != '-' &&
           *str != '+')
        ++str;
    return str;
}

const char* getQuotedZoneName(const char* str, char delimiter) {
    while (*str != '\0' && *str != delimiter)
        ++str;
    return str;
}

const char* getNumber(const char* str, i32& out, i32 min, i32 max) {
    if (str == nullptr || !isDigit(*str))
        return nullptr;
    i32 num = 0;
    do {
        num = num * 10 + (*str - '0');
        if (num > max)
            return nullptr;
        ++str;
    } while (isDigit(*str));
    if (num < min)
        return nullptr;
    out = num;
    return str;
}

const char* getSeconds(const char* str, i32& out) {
    i32 num;
    str = getNumber(str, num, 0, 24 * DAYS_PER_WEEK - 1);
    if (str == nullptr)
        return nullptr;
    out = num * SECONDS_PER_HOUR;
    if (*str == ':') {
        str = getNumber(str + 1, num, 0, 59);
        if (str == nullptr)
            return nullptr;
        out += num * SECONDS_PER_MINUTE;
        if (*str == ':') {
            // 60 allows for leap seconds
            str = getNumber(str + 1, num, 0, 60);
            if (str == nullptr)
                return nullptr;
            out += num;
        }
    }
    return str;
}

const char* getOffset(const char* str, i32& out) {
    bool negative = false;
    if (*str == '-') {
        negative = true;
        ++str;
    } else if (*str == '+') {
        ++str;
    }
    str = getSeconds(str, out);
    if (str == nullptr)
        return nullptr;
    if (negative)
        out = -out;
    return str;
}

const char* getRule(const char* str, PosixRule& rule) {
    if (*str == 'J') {
        rule.type = PosixRuleType::JulianDay;
        str = getNumber(str + 1, rule.day, 1, DAYS_PER_NORMAL_YEAR);
    } else if (*str == 'M') {
        rule.type = PosixRuleType::MonthNthDayOfWeek;
        str = getNumber(str + 1, rule.month, 1, MONTHS_PER_YEAR);
        if (str == nullptr || *str++ != '.')
            return nullptr;
        str = getNumber(str, rule.week, 1, 5);
        if (str == nullptr || *str++ != '.')
            return nullptr;
        str = getNumber(str, rule.day, 0, DAYS_PER_WEEK - 1);
    } else if (isDigit(*str)) {
        rule.type = PosixRuleType::DayOfYear;
        str = getNumber(str, rule.day, 0, DAYS_PER_LEAP_YEAR - 1);
    } else {
        return nullptr;
    }
    if (str == nullptr)
        return nullptr;
    if (*str == '/')
        return getOffset(str + 1, rule.time);
    rule.time = 2 * SECONDS_PER_HOUR;
    return str;
}

// Seconds since the epoch of the rule's transition in a year, given the
// seconds of January 1st 00:00:00 of that year (UTC)
i64 transitionTime(i64 jan_first, i64 year, const PosixRule& rule,
                   i32 offset) {
    const int leap = isLeap(year) ? 1 : 0;
    i64 value = 0;
    switch (rule.type) {
    case PosixRuleType::JulianDay:
        // Jn: 1..365, February 29th is never counted
        value = jan_first + (rule.day - 1) * i64{SECONDS_PER_DAY};
        if (leap != 0 && rule.day >= 60)
            value += SECONDS_PER_DAY;
        break;
    case PosixRuleType::DayOfYear:
        value = jan_first + rule.day * i64{SECONDS_PER_DAY};
        break;
    case PosixRuleType::MonthNthDayOfWeek: {
        value = jan_first;
        for (int i = 0; i < rule.month - 1; ++i)
            value += MONTH_LENGTHS[leap][i] * i64{SECONDS_PER_DAY};

        // Day of the week of the first day of the month (Zeller's congruence)
        const i64 m1 = (rule.month + 9) % 12 + 1;
        const i64 yy0 = rule.month <= 2 ? year - 1 : year;
        const i64 yy1 = yy0 / 100;
        const i64 yy2 = yy0 % 100;
        i64 dow = ((26 * m1 - 2) / 10 + 1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
        if (dow < 0)
            dow += DAYS_PER_WEEK;

        // First day of the month with the requested weekday, then the Nth
        i64 d = rule.day - dow;
        if (d < 0)
            d += DAYS_PER_WEEK;
        for (int i = 1; i < rule.week; ++i) {
            if (d + DAYS_PER_WEEK >= MONTH_LENGTHS[leap][rule.month - 1])
                break;
            d += DAYS_PER_WEEK;
        }
        value += d * SECONDS_PER_DAY;
        break;
    }
    }
    return value + rule.time + offset;
}

struct PosixZone {
    // Standard time first, then daylight saving time
    char names[2][TimeZoneRule::MAX_NAME_LEN + 1];
    i32 std_offset; // seconds west of UTC, as in the string
    i32 dst_offset;
    PosixRule start;
    PosixRule end;
};

// Parses a name followed by an optional offset. Returns the position after it
// or nullptr.
const char* getName(const char* str, char (&out)[TimeZoneRule::MAX_NAME_LEN + 1]) {
    const char* begin;
    const char* end;
    if (*str == '<') {
        begin = ++str;
        end = str = getQuotedZoneName(str, '>');
        if (*str != '>')
            return nullptr;
        ++str;
    } else {
        begin = str;
        end = str = getZoneName(str);
    }
    const auto length = static_cast<usize>(end - begin);
    if (length > TimeZoneRule::MAX_NAME_LEN)
        return nullptr;
    std::memcpy(out, begin, length);
    out[length] = '\0';
    return str;
}

// Only rules with daylight saving time and explicit start/end dates produce
// transitions. Strings without DST ("JST-9") describe a single type that the
// file already contains, and DST names without dates would need the system's
// default rules (posixrules), which the console does not have either.
bool parsePosixZone(const char* str, PosixZone& zone) {
    str = getName(str, zone.names[0]);
    if (str == nullptr || std::strlen(zone.names[0]) < 3 || *str == '\0')
        return false;
    str = getOffset(str, zone.std_offset);
    if (str == nullptr || *str == '\0')
        return false;

    str = getName(str, zone.names[1]);
    if (str == nullptr)
        return false;
    if (*str != '\0' && *str != ',' && *str != ';') {
        str = getOffset(str, zone.dst_offset);
        if (str == nullptr)
            return false;
    } else {
        zone.dst_offset = zone.std_offset - SECONDS_PER_HOUR;
    }

    if (*str != ',' && *str != ';')
        return false;
    str = getRule(str + 1, zone.start);
    if (str == nullptr || *str++ != ',')
        return false;
    str = getRule(str, zone.end);
    return str != nullptr && *str == '\0';
}

// Appends the transitions of the footer rule after the last stored one, like
// tzload() does with its extension. Returns false if nothing was added.
bool extendWithPosixRule(TimeZoneRule& rule, const char* footer) {
    PosixZone zone;
    if (!parsePosixZone(footer, zone))
        return false;

    const auto std_length = std::strlen(zone.names[0]);
    const auto dst_length = std::strlen(zone.names[1]);
    const usize char_count = std_length + 1 + dst_length + 1;
    if (rule.type_count + 2 > TimeZoneRule::MAX_TYPE_COUNT ||
        rule.char_count + char_count > TimeZoneRule::MAX_CHAR_COUNT)
        return false;

    // Standard time is type 0 and daylight saving time type 1 of the rule
    const u32 std_type = rule.type_count;
    const u32 dst_type = rule.type_count + 1;
    TimeTypeInfo& std_info = rule.type_infos[std_type];
    TimeTypeInfo& dst_info = rule.type_infos[dst_type];
    std_info = {};
    dst_info = {};
    std_info.gmt_offset = -zone.std_offset;
    std_info.is_day_saving_time = false;
    std_info.abbreviation_list_index = rule.char_count;
    dst_info.gmt_offset = -zone.dst_offset;
    dst_info.is_day_saving_time = true;
    dst_info.abbreviation_list_index =
        rule.char_count + static_cast<u32>(std_length) + 1;

    // Generate the transitions of each year from 1970 until the table is full
    i64 ats[TimeZoneRule::MAX_TIME_COUNT];
    u8 types[TimeZoneRule::MAX_TIME_COUNT];
    u32 count = 0;
    i64 jan_first = 0;
    for (i64 year = EPOCH_YEAR; count + 2 <= TimeZoneRule::MAX_TIME_COUNT;
         ++year) {
        const i64 start_time =
            transitionTime(jan_first, year, zone.start, zone.std_offset);
        const i64 end_time =
            transitionTime(jan_first, year, zone.end, zone.dst_offset);
        if (start_time > end_time) {
            // Southern hemisphere: daylight saving time spans the new year
            ats[count] = end_time;
            types[count++] = 0;
            ats[count] = start_time;
            types[count++] = 1;
        } else {
            ats[count] = start_time;
            types[count++] = 1;
            ats[count] = end_time;
            types[count++] = 0;
        }
        jan_first += (isLeap(year) ? DAYS_PER_LEAP_YEAR : DAYS_PER_NORMAL_YEAR) *
                     i64{SECONDS_PER_DAY};
    }

    u32 i = 0;
    if (rule.time_count != 0) {
        while (i < count && ats[i] <= rule.ats[rule.time_count - 1])
            ++i;
    }
    while (i < count && rule.time_count < TimeZoneRule::MAX_TIME_COUNT) {
        rule.ats[rule.time_count] = ats[i];
        rule.type_indices[rule.time_count] =
            static_cast<u8>(types[i] == 0 ? std_type : dst_type);
        ++rule.time_count;
        ++i;
    }

    std::memcpy(rule.chars + rule.char_count, zone.names[0], std_length + 1);
    std::memcpy(rule.chars + rule.char_count + std_length + 1, zone.names[1],
                dst_length + 1);
    rule.char_count += static_cast<u32>(char_count);
    rule.chars[rule.char_count] = '\0';
    rule.type_count += 2;
    return true;
}

} // namespace

// From Ryujinx
void parseTimeZoneBinary(ztd::io::IStream* stream, TimeZoneRule& out_rule) {
    const auto header = stream->read<TzifHeader>();
    ASSERT(header.magic == makeMagic4('T', 'Z', 'i', 'f'), Services,
           "Invalid TZif magic {:#x}", header.magic);

    u32 data_size = static_cast<u32>(stream->getRemainingSize());

    u32 ttis_gmt_count = decode(header.ttis_gmt_count);
    u32 ttis_std_count = decode(header.ttis_std_count);
    u32 leap_count = decode(header.leap_count);
    u32 time_count = decode(header.time_count);
    u32 type_count = decode(header.type_count);
    u32 char_count = decode(header.char_count);

    ASSERT(leap_count < TimeZoneRule::MAX_LEAP_COUNT &&
               type_count < TimeZoneRule::MAX_TYPE_COUNT &&
               time_count < TimeZoneRule::MAX_TIME_COUNT &&
               char_count < TimeZoneRule::MAX_CHAR_COUNT &&
               (ttis_std_count == type_count || ttis_std_count == 0) &&
               (ttis_gmt_count == type_count || ttis_gmt_count == 0),
           Services, "Invalid header parameters");
    ASSERT((time_count * sizeof(u64) + time_count +
            static_cast<usize>(type_count) * 6 + char_count +
            leap_count * (sizeof(u64) + 4) + ttis_std_count + ttis_gmt_count) <=
               data_size,
           Services, "Insufficient data size");

    out_rule.time_count = time_count;
    out_rule.type_count = type_count;
    out_rule.char_count = char_count;

    time_count = 0;

    for (u32 i = 0; i < out_rule.time_count; i++) {
        const auto at = decode(stream->read<i64>());
        out_rule.type_indices[i] = 1;

        if (time_count != 0 && at <= out_rule.ats[time_count - 1]) {
            ASSERT(at >= out_rule.ats[time_count - 1], Services,
                   "Invalid at ({} < {})", at, out_rule.ats[time_count - 1]);

            out_rule.type_indices[i - 1] = 0;
            time_count--;
        }

        out_rule.ats[time_count++] = at;
    }

    time_count = 0;
    for (u32 i = 0; i < out_rule.time_count; i++) {
        const auto type_index = stream->read<u8>();
        ASSERT(type_index < out_rule.type_count, Services,
               "Invalid type index ({} >= {})", type_index,
               out_rule.type_count);

        if (out_rule.type_indices[i] != 0)
            out_rule.type_indices[time_count++] = type_index;
    }

    out_rule.time_count = time_count;

    for (u32 i = 0; i < out_rule.type_count; i++) {
        TimeTypeInfo& type_info = out_rule.type_infos[i];
        type_info.gmt_offset = decode(stream->read<i32>());

        const auto is_day_saving_time = stream->read<u8>();
        ASSERT(is_day_saving_time < 2, Services,
               "Invalid is day saving time boolean {}", is_day_saving_time);

        type_info.is_day_saving_time = (is_day_saving_time != 0);

        u32 abbreviation_list_index = stream->read<u8>();
        ASSERT(abbreviation_list_index < TimeZoneRule::MAX_CHAR_COUNT, Services,
               "Invalid abbreviation list index {}", abbreviation_list_index);

        type_info.abbreviation_list_index = abbreviation_list_index;
    }

    stream->readToSpan(std::span<char>(out_rule.chars, out_rule.char_count));
    out_rule.chars[out_rule.char_count] = '\0';

    for (u32 i = 0; i < out_rule.type_count; i++) {
        if (ttis_std_count == 0) {
            out_rule.type_infos[i].is_standard_time_daylight = false;
        } else {
            const auto is_standard_time_daylight = stream->read<u8>();
            ASSERT(is_standard_time_daylight < 2, Services,
                   "Invalid is standard time daylight boolean {}",
                   is_standard_time_daylight);

            out_rule.type_infos[i].is_standard_time_daylight =
                (is_standard_time_daylight != 0);
        }
    }

    for (u32 i = 0; i < out_rule.type_count; i++) {
        if (ttis_std_count == 0) {
            out_rule.type_infos[i].is_gmt = false;
        } else {
            const auto is_gmt = stream->read<u8>();
            ASSERT(is_gmt < 2, Services, "Invalid is GMT boolean {}", is_gmt);

            out_rule.type_infos[i].is_gmt = (is_gmt != 0);
        }
    }

    u32 name_len = static_cast<u32>(stream->getRemainingSize());
    ASSERT(name_len <= (TimeZoneRule::MAX_NAME_LEN + 1), Services,
           "Invalid name length {}", name_len);

    char tmp_name[TimeZoneRule::MAX_NAME_LEN + 1];
    stream->readToSpan(std::span<char>(tmp_name, name_len));

    if (name_len > 2 && tmp_name[0] == '\n' && tmp_name[name_len - 1] == '\n' &&
        out_rule.type_count + 2 <= TimeZoneRule::MAX_TYPE_COUNT) {
        tmp_name[name_len - 1] = '\0';

        // tmp_name[0] is the newline before the POSIX TZ string
        if (!extendWithPosixRule(out_rule, tmp_name + 1))
            LOG_DEBUG(Services, "Ignoring time zone footer \"{}\"",
                      tmp_name + 1);
    }

    ASSERT(out_rule.type_count > 0, Services, "Invalid type count");

    if (out_rule.time_count > 1) {
        for (u32 i = 1; i < out_rule.time_count; i++) {
            if (timeTypeEquals(out_rule, out_rule.type_indices[i],
                               out_rule.type_indices[0]) &&
                differByRepeat(out_rule.ats[i], out_rule.ats[0])) {
                out_rule.go_back = true;
                break;
            }
        }

        for (i32 i = static_cast<i32>(out_rule.time_count - 2); i >= 0; i--) {
            if (timeTypeEquals(out_rule,
                               out_rule.type_indices[out_rule.time_count - 1],
                               out_rule.type_indices[i]) &&
                differByRepeat(out_rule.ats[out_rule.time_count - 1],
                               out_rule.ats[i])) {
                out_rule.go_ahead = true;
                break;
            }
        }
    }

    u32 default_type;
    for (default_type = 0; default_type < out_rule.time_count; default_type++) {
        if (out_rule.type_indices[default_type] == 0) {
            break;
        }
    }

    if (default_type < out_rule.time_count) {
        if (out_rule.time_count > 0 &&
            out_rule.type_infos[out_rule.type_indices[0]].is_day_saving_time) {
            default_type = out_rule.type_indices[0];
            while (true) {
                if (default_type > 0) {
                    default_type--;
                    if (!out_rule.type_infos[default_type].is_day_saving_time) {
                        break;
                    }
                } else {
                    default_type = 0;
                    while (
                        out_rule.type_infos[default_type].is_day_saving_time) {
                        if (++default_type >= out_rule.type_count) {
                            default_type = 0;
                            break;
                        }
                    }
                }
            }
        }

        out_rule.default_type = default_type;
    } else {
        out_rule.default_type = 0;
    }
}

} // namespace hydra::horizon::services::timesrv::internal

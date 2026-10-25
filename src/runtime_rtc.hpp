#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef __cplusplus
namespace bb::rtc {

struct DateTime {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint32_t microsecond;
};
static_assert(sizeof(DateTime) == 16, "OrbisRtcDateTime layout must be 16 bytes");
static_assert(std::is_trivial_v<DateTime>, "DateTime must be trivial");

constexpr int32_t ERR_INVALID_POINTER = static_cast<int32_t>(0x80B50002);
constexpr int32_t ERR_INVALID_VALUE = static_cast<int32_t>(0x80B50003);
constexpr int32_t ERR_INVALID_YEAR = static_cast<int32_t>(0x80B50008);
constexpr int32_t ERR_INVALID_MONTH = static_cast<int32_t>(0x80B50009);
constexpr int32_t ERR_INVALID_DAY = static_cast<int32_t>(0x80B5000A);
constexpr int32_t ERR_INVALID_HOUR = static_cast<int32_t>(0x80B5000B);
constexpr int32_t ERR_INVALID_MINUTE = static_cast<int32_t>(0x80B5000C);
constexpr int32_t ERR_INVALID_SECOND = static_cast<int32_t>(0x80B5000D);
constexpr int32_t ERR_INVALID_MICROSECOND = static_cast<int32_t>(0x80B5000E);

constexpr uint64_t kUnixEpochTicks = UINT64_C(0xdcbffeff2bc000);
constexpr uint64_t kTicksPerDay = UINT64_C(86400000000);

constexpr bool IsLeap(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr int DaysInMonth(int y, int m) noexcept {
    constexpr int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && IsLeap(y)) ? 29 : d[m - 1];
}

constexpr int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) noexcept {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 306;
}

constexpr void CivilFromDays(int64_t z, int &y, int &m, int &d) noexcept {
    z += 306;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

constexpr int32_t Validate(const DateTime *t) noexcept {
    if (!t) return ERR_INVALID_POINTER;
    if (t->year < 1 || t->year > 9999) return ERR_INVALID_YEAR;
    if (t->month < 1 || t->month > 12) return ERR_INVALID_MONTH;
    if (t->day < 1 || t->day > DaysInMonth(t->year, t->month)) return ERR_INVALID_DAY;
    if (t->hour > 23) return ERR_INVALID_HOUR;
    if (t->minute > 59) return ERR_INVALID_MINUTE;
    if (t->second > 59) return ERR_INVALID_SECOND;
    if (t->microsecond > 999999) return ERR_INVALID_MICROSECOND;
    return 0;
}

constexpr uint64_t ToTick(const DateTime *t) noexcept {
    return static_cast<uint64_t>(DaysFromCivil(t->year, t->month, t->day)) * kTicksPerDay +
           (static_cast<uint64_t>(t->hour) * 3600 + static_cast<uint64_t>(t->minute) * 60 + t->second) * 1000000 +
           t->microsecond;
}

constexpr void FromTick(uint64_t tick, DateTime *t) noexcept {
    int y = 0, m = 0, d = 0;
    CivilFromDays(static_cast<int64_t>(tick / kTicksPerDay), y, m, d);
    const uint64_t us = tick % kTicksPerDay;
    t->year = static_cast<uint16_t>(y);
    t->month = static_cast<uint16_t>(m);
    t->day = static_cast<uint16_t>(d);
    t->hour = static_cast<uint16_t>(us / 3600000000u);
    t->minute = static_cast<uint16_t>(us / 60000000u % 60);
    t->second = static_cast<uint16_t>(us / 1000000u % 60);
    t->microsecond = static_cast<uint32_t>(us % 1000000u);
}

} // namespace bb::rtc
#endif

#ifdef __cplusplus
extern "C" {
#endif

void runtime_rtc_register(void);
uintptr_t runtime_rtc_resolve(const char *name);

#ifdef __cplusplus
}
#endif

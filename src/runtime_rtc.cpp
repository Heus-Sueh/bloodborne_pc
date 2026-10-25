#include "runtime_rtc.hpp"
#include "runtime.h"
#include "runtime_registry.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace bb::rtc {

static uint64_t NowUtc() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return kUnixEpochTicks + static_cast<uint64_t>(ts.tv_sec) * 1000000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

static int64_t LocalOffset(uint64_t utc) {
    const auto seconds = static_cast<time_t>(static_cast<int64_t>(utc - kUnixEpochTicks) / 1000000);
    struct tm local{};
    localtime_r(&seconds, &local);
    return static_cast<int64_t>(RUNTIME_TM_GMTOFF(local)) * 1000000;
}

} // namespace bb::rtc

using namespace bb::rtc;

extern "C" {

static ABI int32_t rtc_current_local(DateTime *t) {
    if (!t) return ERR_INVALID_POINTER;
    const uint64_t utc = NowUtc();
    FromTick(static_cast<uint64_t>(static_cast<int64_t>(utc) + LocalOffset(utc)), t);
    return 0;
}

static ABI int32_t rtc_current_clock(DateTime *t, int32_t minutes) {
    if (!t) return ERR_INVALID_POINTER;
    FromTick(static_cast<uint64_t>(static_cast<int64_t>(NowUtc()) + static_cast<int64_t>(minutes) * 60000000), t);
    return 0;
}

static ABI int32_t rtc_current_tick(uint64_t *tick) {
    if (!tick) return ERR_INVALID_POINTER;
    *tick = NowUtc();
    return 0;
}

static ABI int32_t rtc_utc_to_local(const uint64_t *utc, uint64_t *local) {
    if (!utc || !local) return ERR_INVALID_POINTER;
    *local = static_cast<uint64_t>(static_cast<int64_t>(*utc) + LocalOffset(*utc));
    return 0;
}

static ABI int32_t rtc_local_to_utc(const uint64_t *local, uint64_t *utc) {
    if (!local || !utc) return ERR_INVALID_POINTER;
    *utc = static_cast<uint64_t>(static_cast<int64_t>(*local) - LocalOffset(*local));
    return 0;
}

static ABI int32_t rtc_day_of_week(int32_t year, int32_t month, int32_t day) {
    if (year < 1 || year > 9999) return ERR_INVALID_YEAR;
    if (month < 1 || month > 12) return ERR_INVALID_MONTH;
    if (day < 1 || day > DaysInMonth(year, month)) return ERR_INVALID_DAY;
    return static_cast<int32_t>((DaysFromCivil(year, month, day) + 1) % 7);
}

static ABI int32_t rtc_get_tick(const DateTime *t, uint64_t *tick) {
    if (!t || !tick) return ERR_INVALID_POINTER;
    const int32_t e = Validate(t);
    if (e != 0) return e;
    *tick = ToTick(t);
    return 0;
}

static ABI int32_t rtc_set_tick(DateTime *t, const uint64_t *tick) {
    if (!t || !tick) return ERR_INVALID_POINTER;
    FromTick(*tick, t);
    return 0;
}

static ABI uint32_t rtc_tick_resolution(void) {
    return 1000000;
}

static ABI int32_t rtc_set_time_t(DateTime *t, int64_t seconds) {
    if (!t) return ERR_INVALID_POINTER;
    if (seconds < 0) return ERR_INVALID_VALUE;
    FromTick(kUnixEpochTicks + static_cast<uint64_t>(seconds) * 1000000u, t);
    return 0;
}

static ABI int32_t rtc_get_time_t(const DateTime *t, int64_t *seconds) {
    if (!t || !seconds) return ERR_INVALID_POINTER;
    const int32_t e = Validate(t);
    if (e != 0) return e;
    const uint64_t tick = ToTick(t);
    *seconds = tick < kUnixEpochTicks ? 0 : static_cast<int64_t>((tick - kUnixEpochTicks) / 1000000);
    return 0;
}

static ABI int32_t rtc_rfc2822_local(char *out, const uint64_t *utc_tick) {
    if (!out) return ERR_INVALID_POINTER;
    const uint64_t utc = utc_tick ? *utc_tick : NowUtc();
    const int64_t offset = LocalOffset(utc);
    DateTime t{};
    FromTick(static_cast<uint64_t>(static_cast<int64_t>(utc) + offset), &t);

    static constexpr const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int64_t minutes = offset / 60000000;
    const int dow = static_cast<int>((DaysFromCivil(t.year, t.month, t.day) + 1) % 7);
    const int abs_mins = static_cast<int>(minutes < 0 ? -minutes : minutes);

    char text[64];
    std::snprintf(text, sizeof(text), "%s, %02u %s %04u %02u:%02u:%02u %c%02d%02d",
                  days[dow], t.day,
                  months[t.month - 1], t.year, t.hour, t.minute, t.second,
                  minutes < 0 ? '-' : '+', abs_mins / 60, abs_mins % 60);
    std::memcpy(out, text, std::strlen(text) + 1);
    return 0;
}

static const RuntimeExport rtc_exports[] = {
    {"sceRtcGetCurrentClockLocalTime", reinterpret_cast<void*>(rtc_current_local)},
    {"sceRtcGetCurrentClock", reinterpret_cast<void*>(rtc_current_clock)},
    {"sceRtcGetCurrentTick", reinterpret_cast<void*>(rtc_current_tick)},
    {"sceRtcGetCurrentNetworkTick", reinterpret_cast<void*>(rtc_current_tick)},
    {"sceRtcConvertUtcToLocalTime", reinterpret_cast<void*>(rtc_utc_to_local)},
    {"sceRtcConvertLocalTimeToUtc", reinterpret_cast<void*>(rtc_local_to_utc)},
    {"sceRtcGetDayOfWeek", reinterpret_cast<void*>(rtc_day_of_week)},
    {"sceRtcGetTick", reinterpret_cast<void*>(rtc_get_tick)},
    {"sceRtcSetTick", reinterpret_cast<void*>(rtc_set_tick)},
    {"sceRtcGetTickResolution", reinterpret_cast<void*>(rtc_tick_resolution)},
    {"sceRtcSetTime_t", reinterpret_cast<void*>(rtc_set_time_t)},
    {"sceRtcGetTime_t", reinterpret_cast<void*>(rtc_get_time_t)},
    {"sceRtcFormatRFC2822LocalTime", reinterpret_cast<void*>(rtc_rfc2822_local)},
};

void runtime_rtc_register(void) {
    runtime_register_exports(rtc_exports, sizeof(rtc_exports) / sizeof(rtc_exports[0]));
}

uintptr_t runtime_rtc_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

} // extern "C"

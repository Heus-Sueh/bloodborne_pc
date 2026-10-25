#ifndef BB_PLATFORM_TIME_H
#define BB_PLATFORM_TIME_H

#include "platform.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic steady clock timestamp in nanoseconds. */
uint64_t bb_platform_monotonic_ns(void);

/* Realtime clock timestamp in nanoseconds since UNIX epoch (1970-01-01). */
uint64_t bb_platform_realtime_ns(void);

/* Sleeps the calling thread for the specified duration in nanoseconds. */
void bb_platform_sleep_ns(uint64_t ns);

/* High-precision sleep until the monotonic clock reaches deadline_ns. */
void bb_platform_sleep_until_ns(uint64_t deadline_ns);

/* Local time offset east of UTC in seconds (e.g. +3600 for UTC+1). */
int64_t bb_platform_utc_offset(time_t when);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_TIME_H */

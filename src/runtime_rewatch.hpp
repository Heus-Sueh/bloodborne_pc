#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_re_watch_now(uint64_t address);
void runtime_re_watch_report(void);
void runtime_re_watch(uint64_t address);
void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads);

#ifdef __cplusplus
}
#endif

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_kernel_register(void);
uintptr_t runtime_kernel_resolve(const char *name);

uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
void runtime_sleep_stats(uint64_t *calls, uint64_t *ns);
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
void runtime_thread_keys_cleanup(void);

#ifdef __cplusplus
}
#endif

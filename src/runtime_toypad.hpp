#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_toypad_register(void);
uintptr_t runtime_toypad_resolve(const char *name);
void runtime_toypad_lights(uint32_t out[3]);
void runtime_toypad_report(void);

#ifdef __cplusplus
}
#endif

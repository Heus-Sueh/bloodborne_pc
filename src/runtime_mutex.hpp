#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_mutex_register(void);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);

#ifdef __cplusplus
}
#endif

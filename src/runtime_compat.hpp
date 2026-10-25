#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_compat_register(void);
uintptr_t runtime_compat_resolve(const char *name);
void runtime_compat_report(void);

#ifdef __cplusplus
}
#endif

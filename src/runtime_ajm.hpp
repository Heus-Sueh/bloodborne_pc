#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_ajm_register(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);

#ifdef __cplusplus
}
#endif

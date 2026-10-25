#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_ngs2_register(void);
uintptr_t runtime_ngs2_resolve(const char *name);
void runtime_ngs2_report(void);

#ifdef __cplusplus
}
#endif

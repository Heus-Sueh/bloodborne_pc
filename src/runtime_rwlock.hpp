#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_rwlock_register(void);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);

#ifdef __cplusplus
}
#endif

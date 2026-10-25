#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_sema_register(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);

#ifdef __cplusplus
}
#endif

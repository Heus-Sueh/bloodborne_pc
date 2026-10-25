#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_services_register(void);
uintptr_t runtime_services_resolve(const char *name);

#ifdef __cplusplus
}
#endif

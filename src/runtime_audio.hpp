#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_audio_register(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);

#ifdef __cplusplus
}
#endif

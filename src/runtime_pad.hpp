#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_pad_register(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);

int runtime_pad_prompt_device(void);
unsigned int runtime_pad_prompt_generation(void);
int runtime_pad_mouse_look(void);
int runtime_pad_prompt(const char *name, int prefer_mouse, char *label, int size);

#ifdef __cplusplus
}
#endif

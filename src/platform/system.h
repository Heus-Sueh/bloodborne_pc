#ifndef BB_PLATFORM_SYSTEM_H
#define BB_PLATFORM_SYSTEM_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills buffer with high-entropy cryptographic random bytes. Returns 0 on success, -1 on error. */
int bb_platform_random(void *buffer, size_t size);

/* Sets the native thread name for host debuggers and profilers. */
void bb_platform_set_thread_name(const char *name);

/* Configures the guest Thread Control Block (TCB) pointer on the current host thread. */
void bb_platform_set_guest_tcb(void *tcb);

/* Returns system page size in bytes (e.g. 4096 or 16384). */
size_t bb_platform_page_size(void);

/* Returns total logical CPU count on the host system. */
int bb_platform_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_SYSTEM_H */

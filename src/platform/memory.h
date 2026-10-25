#ifndef BB_PLATFORM_MEMORY_H
#define BB_PLATFORM_MEMORY_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>

#define BB_PROT_NONE  0x00
#define BB_PROT_READ  0x01
#define BB_PROT_WRITE 0x02
#define BB_PROT_EXEC  0x04

#ifdef __cplusplus
extern "C" {
#endif

/* Reserves virtual address space span [base, base + size) without committing memory. */
void *bb_platform_reserve_space(uintptr_t base, size_t size);

/* Commits anonymous private memory at [base, base + size). */
void *bb_platform_alloc_committed(uintptr_t base, size_t size, int prot);

/* Updates memory protection on range [addr, addr + size). */
int bb_platform_protect(void *addr, size_t size, int prot);

/* Releases committed virtual memory pages in [addr, addr + size). */
int bb_platform_release(void *addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_MEMORY_H */

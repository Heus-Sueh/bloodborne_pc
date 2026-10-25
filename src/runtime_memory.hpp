#pragma once

#include "runtime.h"
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GpuRange)(uintptr_t address, uint64_t size);

void runtime_memory_register(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
void runtime_memory_reserve(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_check_mapped(uintptr_t address, uint64_t size);
void runtime_memory_set_coherence_hooks(const RuntimeMemoryCoherenceHooks *hooks);
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate);
void runtime_memory_set_note_write_hook(GpuRange note);
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);
void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on);
unsigned int runtime_memory_trap_reasons(uintptr_t address);
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end);
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
const uint64_t *runtime_memory_generation(void);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
void runtime_memory_set_guest_chunk_whole(int whole);
void *runtime_memory_backing_pointer(uint64_t phys);
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size));
void runtime_memory_set_cpu_write_hook(GpuRange hook);
int runtime_extra_direct_map(uint64_t size, uint64_t alignment, void **out);
void *runtime_low_map(size_t size, int prot);
uint64_t runtime_heap_growths(void);

extern uint64_t runtime_disabled_optimizations;
extern uint64_t runtime_experiment_bits;

#ifdef __cplusplus
}
#endif

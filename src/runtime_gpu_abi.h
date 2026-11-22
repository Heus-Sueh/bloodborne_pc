// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Single compiler-checked C/C++ ABI contract for symbols exported by the C runtime (src/)
 * and called across the seam by libbbgpu / libbbnet20 (gpu/) and GPU test stubs (tests/).
 * Every defining .c file in src/, every consumer in gpu/, and tests/test_gpu_runtime_stubs.cpp
 * includes this header so signatures and variable widths are enforced at compile time. */
#ifndef BB_RUNTIME_GPU_ABI_H
#define BB_RUNTIME_GPU_ABI_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#ifndef BB_RUNTIME_SYSV_ABI
#define BB_RUNTIME_SYSV_ABI __attribute__((sysv_abi))
#endif

#if defined(__cplusplus) && !defined(_WIN32)
#define BB_RUNTIME_OPTIONAL_WEAK __attribute__((weak))
#else
#define BB_RUNTIME_OPTIONAL_WEAK
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);

/* --- Runtime toggles, experiments, fault recovery & restart (probe.c, runtime_memory/) --- */
extern uint64_t runtime_disabled_optimizations;
extern uint64_t runtime_experiment_bits;
extern __thread sigjmp_buf *runtime_fault_recover;
extern volatile int runtime_restarting BB_RUNTIME_OPTIONAL_WEAK;

void runtime_restart(void) BB_RUNTIME_OPTIONAL_WEAK;

/* --- Guest memory, backing store, VMA queries & traps (src/runtime_memory/) --- */
void runtime_memory_reserve(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
const uint64_t *runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);

void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
void runtime_memory_set_host_only_allocations(uint64_t mask);

void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);

void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on);
unsigned runtime_memory_trap_reasons(uintptr_t address);
void runtime_memory_trap_stats(uint64_t *wait_ns, uint64_t *apply_ns, uint64_t *calls, uint64_t *bytes);

int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end);
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end);
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end);
void *runtime_low_map(size_t size, int prot);
int runtime_extra_direct_map(uint64_t size, uint64_t alignment, void **out);

void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size));
void runtime_memory_set_guest_chunk_whole(int whole);
void runtime_memory_set_guest_chunk_private(int (*import_fn)(uint64_t phys, uint64_t size, void *pointer));
void runtime_memory_set_guest_chunk_free(void (*free_chunk)(uint64_t phys, uint64_t size));
void runtime_memory_set_guest_lazy(int lazy);
void *runtime_memory_backing_pointer(uint64_t phys);
int runtime_memory_pool_fd(void);
void runtime_memory_note_pinned(uint64_t phys, uint64_t size);
void runtime_memory_discard_direct(uint64_t phys, uint64_t size);

void runtime_memory_set_grow(const uint64_t *size_at, uint64_t initial);
int runtime_memory_deferred(uint64_t *phys, uint64_t *end);
int runtime_memory_commit(uint64_t phys, uint64_t size);
int runtime_memory_decommit(uint64_t phys, uint64_t size);
void runtime_memory_commit_ahead(uint64_t phys, uint64_t size);
uintptr_t runtime_memory_resolve(const char *name);
uint64_t runtime_heap_growths(void);

/* --- Clock, threads, errno & wait diagnostics (runtime_kernel.c, runtime_thread.c) --- */
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t *runtime_errno(void);
void runtime_wait_report(double frames);
void runtime_sleep_stats(uint64_t *calls, uint64_t *ns);
void runtime_thread_attach_host(const char *name);
int32_t runtime_thread_spawn(void **thread, void *(BB_RUNTIME_SYSV_ABI *entry)(void *),
                             void *argument, uint64_t stack, const char *name);

/* --- Files, savedata, pad prompts & RE watches (runtime_file.c, runtime_savedata.c, runtime_pad/, runtime_rewatch.c) --- */
int runtime_file_translate(const char *guest, char *out, size_t size);
int runtime_file_host_path(const char *guest, char *out, size_t size);
const char *runtime_file_user_dir(void);
int runtime_savedata_active_slot(char *dir, size_t size);

int runtime_pad_prompt_device(void);
unsigned runtime_pad_prompt_generation(void);
int runtime_pad_mouse_look(void);
int runtime_pad_prompt(const char *name, int prefer_mouse, char *label, int size);
int runtime_pad_steam_only(void) BB_RUNTIME_OPTIONAL_WEAK;

void runtime_re_watch(uint64_t address);

#ifdef __cplusplus
}
#endif

#endif

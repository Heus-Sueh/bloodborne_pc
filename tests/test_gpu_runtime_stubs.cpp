// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime_gpu_abi.h"

#include <chrono>
#include <cstdlib>

// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
// Every symbol below is checked at compile time against src/runtime_gpu_abi.h.
extern "C" {
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
volatile int runtime_restarting = 0;

uint64_t runtime_tsc_frequency(void) { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us(void) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
int runtime_memory_is_mapped(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_gpu_hooks(RuntimeGpuRange, RuntimeGpuRange, RuntimeGpuRange) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart(void) { std::abort(); }
uint64_t runtime_process_time_counter(void) { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno(void) { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation(void) { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(RuntimeGpuRange) {}
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
void runtime_memory_trap(uintptr_t, uint64_t, unsigned, int) { std::abort(); }
unsigned runtime_memory_trap_reasons(uintptr_t) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void* runtime_memory_backing_pointer(uint64_t) { std::abort(); }
uintptr_t runtime_memory_resolve(const char*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths(void) { return 0; }

// Input prompts and reverse-engineering watches (no pad, no game files here).
int runtime_pad_prompt_device(void) { return 0; }
unsigned runtime_pad_prompt_generation(void) { return 0; }
int runtime_pad_mouse_look(void) { return 0; }
int runtime_pad_prompt(const char*, int, char*, int) { return 0; }
int runtime_pad_steam_only(void) { return 0; }
int runtime_file_host_path(const char*, char*, size_t) { return -1; }
void runtime_re_watch(uint64_t) {}
}

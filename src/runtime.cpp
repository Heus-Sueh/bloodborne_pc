/* Narrow, explicit PS4 libc contracts. No automatic success stubs. */
#define _CRT_RAND_S
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime.h"
#include "core/bbgpu.h"
#include "src/runtime_ajm.hpp"
#include "src/runtime_audio.hpp"
#include "src/runtime_compat.hpp"
#include "src/runtime_content.hpp"
#include "src/runtime_file.hpp"
#include "src/runtime_memory.hpp"
#include "src/runtime_mutex.hpp"
#include "src/runtime_net.hpp"
#include "src/runtime_ngs2.hpp"
#include "src/runtime_pad.hpp"
#include "src/runtime_rwlock.hpp"
#include "src/runtime_savedata.hpp"
#include "src/runtime_sema.hpp"
#include "src/runtime_toypad.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>

extern "C" {
void runtime_thread_report(void);
}

namespace {

struct ExitHandler {
    union {
        GuestCallback plain;
        void (ABI *with_arg)(void *);
    } callback{};
    void *argument{nullptr};
    void *dso{nullptr};
    int with_arg{0};
    int active{0};
};

ExitHandler *handlers = nullptr;
size_t handler_count = 0;
size_t handler_capacity = 0;
size_t calls_init = 0;
size_t calls_atexit = 0;
size_t calls_cxa = 0;
uint64_t capabilities = 0;
std::atomic<size_t> guards_acquired{0};
std::atomic<size_t> guards_released{0};
uint64_t stack_canary = 0;
std::atomic<size_t> memory_calls{0};

/* Dynamic TLS of linked modules: module IDs 2..7 (1 is the eboot's static TLS). */
constexpr size_t TLS_MODULES = 8;
struct TlsModule {
    const void *data{nullptr};
    uint64_t filesz{0};
    uint64_t memsz{0};
};
TlsModule tls_modules[TLS_MODULES];
thread_local unsigned char *tls_blocks[TLS_MODULES] = {nullptr};
void *process_param = nullptr;

void **application_heap_api = nullptr;

ABI void *guest_procparam() {
    return process_param;
}

ABI void guest_set_heap_api(void **api) {
    application_heap_api = api;
    puts("Runtime: application heap API registered");
}

ABI void *guest_tls_get_addr(const uint64_t *index) {
    if (!index || index[0] < 2 || index[0] >= TLS_MODULES || !tls_modules[index[0]].data || index[1] >= tls_modules[index[0]].memsz) {
        fputs("STOP: unsupported TLS module/offset\n", stderr);
        exit(21);
    }
    unsigned char **block = &tls_blocks[index[0]];
    if (!*block) {
        *block = static_cast<unsigned char *>(calloc(1, tls_modules[index[0]].memsz));
        if (!*block) {
            fputs("Cannot allocate module TLS\n", stderr);
            exit(1);
        }
        memcpy(*block, tls_modules[index[0]].data, tls_modules[index[0]].filesz);
    }
    return *block + index[1];
}

ABI void init_env() {
    /* prepare.py verified the supplied libc export consists of exactly C3. */
    ++calls_init;
    puts("Runtime: _init_env returned (verified libc implementation: RET)");
}

std::mutex handler_lock;

int register_handler(ExitHandler value) {
    const std::lock_guard<std::mutex> guard(handler_lock);
    if (handler_count == handler_capacity) {
        size_t capacity = handler_capacity ? handler_capacity * 2 : 64;
        auto *next = (capacity > 1024 * 1024) ? nullptr : static_cast<ExitHandler *>(realloc(handlers, capacity * sizeof(*handlers)));
        if (!next) return -1;
        handlers = next;
        handler_capacity = capacity;
    }
    value.active = 1;
    handlers[handler_count++] = value;
    return 0;
}

ABI int guest_atexit(GuestCallback callback) {
    if (!callback) return -1;
    ++calls_atexit;
    ExitHandler h{};
    h.callback.plain = callback;
    return register_handler(h);
}

ABI int guest_cxa_atexit(void (ABI *callback)(void *), void *argument, void *dso) {
    if (!callback) return -1;
    ++calls_cxa;
    ExitHandler h{};
    h.callback.with_arg = callback;
    h.argument = argument;
    h.dso = dso;
    h.with_arg = 1;
    return register_handler(h);
}

ABI void guest_finalize(void *dso) {
    runtime_finalize(dso);
}

std::atomic<uint32_t> next_guard_owner{1};
thread_local uint32_t guard_owner{0};

ABI int guard_acquire(uint64_t *guard) {
    auto *state = reinterpret_cast<std::atomic<uint64_t> *>(guard);
    /* Dump libc uses the low byte for completion and the dword at +4 as lock.
       The lock word holds a per-thread owner tag so recursion is detectable. */
    if (!guard_owner) guard_owner = next_guard_owner.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
        uint64_t expected = 0;
        if (state->load(std::memory_order_acquire) & 1) return 0;
        if (state->compare_exchange_strong(expected, static_cast<uint64_t>(guard_owner) << 32,
                                           std::memory_order_acq_rel, std::memory_order_acquire)) break;
        if (expected & 1) return 0;
        if (static_cast<uint32_t>(expected >> 32) == guard_owner) {
            fputs("STOP: recursive/concurrent static initialization is not supported yet\n", stderr);
            exit(21);
        }
        sched_yield(); /* another thread is running the initializer */
    }
    guards_acquired.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

ABI void guard_release(uint64_t *guard) {
    auto *state = reinterpret_cast<std::atomic<uint64_t> *>(guard);
    state->store(1, std::memory_order_release);
    guards_released.fetch_add(1, std::memory_order_relaxed);
}

ABI void guard_abort(uint64_t *guard) {
    auto *state = reinterpret_cast<std::atomic<uint64_t> *>(guard);
    state->fetch_and(UINT64_C(0xffffffff), std::memory_order_release);
}

ABI [[noreturn]] void stack_fail() {
    fputs("STOP: guest stack protector detected corruption\n", stderr);
    exit(22);
}

ABI void *guest_memset(void *dst, int value, size_t size) {
    memory_calls.fetch_add(1, std::memory_order_relaxed);
    runtime_memory_prepare_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    std::memset(dst, value, size);
    runtime_memory_note_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    return dst;
}

ABI void *guest_memcpy(void *dst, const void *src, size_t size) {
    memory_calls.fetch_add(1, std::memory_order_relaxed);
    runtime_memory_prepare_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    return dst;
}

ABI void *guest_memmove(void *dst, const void *src, size_t size) {
    memory_calls.fetch_add(1, std::memory_order_relaxed);
    runtime_memory_prepare_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    std::memmove(dst, src, size);
    runtime_memory_note_cpu_write(reinterpret_cast<uintptr_t>(dst), size);
    return dst;
}

ABI int guest_memcmp(const void *a, const void *b, size_t size) {
    memory_calls.fetch_add(1, std::memory_order_relaxed);
    return std::memcmp(a, b, size);
}

ABI size_t guest_strlen(const char *text) {
    return std::strlen(text);
}

ABI [[noreturn]] void guest_libc_exit(int status) {
    printf("Runtime: guest requested exit(%d)\n", status);
    runtime_finalize(nullptr);
    runtime_report();
    exit(status);
}

const char *progname = "eboot.bin";
int32_t need_libc_internal = 1;

const RuntimeExport libc_data_exports[] = {
    {"f7uOxY9mM1U", const_cast<void *>(static_cast<const void *>(&stack_canary))},
    {"ZT4ODD2Ts9o", const_cast<void *>(static_cast<const void *>(&need_libc_internal))},
    {"__progname", const_cast<void *>(static_cast<const void *>(&progname))},
};

const RuntimeExport libc_func_exports[] = {
    {"bzQExy189ZI", reinterpret_cast<void *>(init_env)},
    {"8G2LB+A3rzg", reinterpret_cast<void *>(guest_atexit)},
    {"tsvEmnenz48", reinterpret_cast<void *>(guest_cxa_atexit)},
    {"uMei1W9uyNo", reinterpret_cast<void *>(guest_libc_exit)},
    {"H2e8t5ScQGc", reinterpret_cast<void *>(guest_finalize)},
    {"3GPpjQdAMTw", reinterpret_cast<void *>(guard_acquire)},
    {"9rAeANT2tyE", reinterpret_cast<void *>(guard_release)},
    {"2emaaluWzUw", reinterpret_cast<void *>(guard_abort)},
    {"Ou3iL1abvng", reinterpret_cast<void *>(stack_fail)},
    {"8zTFvBIAIN8", reinterpret_cast<void *>(guest_memset)},
    {"Q3VBxCXhUHs", reinterpret_cast<void *>(guest_memcpy)},
    {"+P6FRGH4LfA", reinterpret_cast<void *>(guest_memmove)},
    {"DfivPArhucg", reinterpret_cast<void *>(guest_memcmp)},
    {"j4ViWNHEgww", reinterpret_cast<void *>(guest_strlen)},
    {"vNe1w4diLCs", reinterpret_cast<void *>(guest_tls_get_addr)},
    {"959qrazPIrg", reinterpret_cast<void *>(guest_procparam)},
    {"p5EcQeEeJAE", reinterpret_cast<void *>(guest_set_heap_api)},
};

} // namespace

extern "C" {

uintptr_t runtime_libc_base = 0;

void runtime_set_procparam(void *param) {
    process_param = param;
    /* bbport: the game's libc heap settings (SceProcParam +0x30: SceLibcParam; its +0x10.. pointers to
     * the heap size, delayed and extended allocation flags, initial size). */
    const auto *p = static_cast<const uint64_t *>(param);
    if (!p || p[0] < 0x38 || !p[6]) return;
    const auto *libc = reinterpret_cast<const uint64_t *>(p[6]);
    printf("Runtime: libc param size %#llx, entries %u", static_cast<unsigned long long>(libc[0]), static_cast<unsigned>(libc[1]));
    static const char *const names[] = {"heap size", "delayed alloc", "extended alloc", "initial size"};
    for (int i = 0; i < 4 && static_cast<uint64_t>(2 + i) * 8 < libc[0]; ++i) {
        const void *field = reinterpret_cast<const void *>(libc[2 + i]);
        if (!field) {
            printf(", %s -", names[i]);
            continue;
        }
        if (i == 0 || i == 3) {
            printf(", %s %#llx", names[i], static_cast<unsigned long long>(*static_cast<const uint64_t *>(field)));
        } else {
            printf(", %s %u", names[i], *static_cast<const uint32_t *>(field));
        }
    }
    printf("\n");
}

void **runtime_application_heap_api() {
    return application_heap_api;
}

void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz) {
    if (module < 2 || module >= TLS_MODULES) {
        fputs("STOP: unsupported TLS module id\n", stderr);
        exit(21);
    }
    tls_modules[module].data = data;
    tls_modules[module].filesz = filesz;
    tls_modules[module].memsz = memsz;
}

void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz) {
    runtime_set_module_tls(2, data, filesz, memsz);
}

void runtime_start(uint64_t flags) {
    capabilities = flags;
    runtime_registry_init();
    if (!(flags & 1)) return;
#ifdef _WIN32
    unsigned int halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) {
        fputs("Cannot initialize stack canary\n", stderr);
        exit(1);
    }
    memcpy(&stack_canary, halves, sizeof(stack_canary));
#else
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random || fread(&stack_canary, sizeof(stack_canary), 1, random) != 1) {
        fputs("Cannot initialize stack canary\n", stderr);
        exit(1);
    }
    fclose(random);
#endif
    stack_canary &= ~UINT64_C(255);
}

void runtime_report() {
    printf("Runtime: _init_env=%zu, atexit=%zu, __cxa_atexit=%zu, registered handlers=%zu\n",
           calls_init, calls_atexit, calls_cxa, handler_count);
    printf("Runtime: static guards acquired=%zu, released=%zu\n",
           guards_acquired.load(std::memory_order_relaxed),
           guards_released.load(std::memory_order_relaxed));
    runtime_thread_report();
    runtime_mutex_report();
    runtime_rwlock_report();
    runtime_sema_report();
    runtime_content_report();
    runtime_memory_report();
    runtime_ajm_report();
    runtime_audio_report();
    runtime_pad_report();
    runtime_savedata_report();
    runtime_compat_report();
    runtime_ngs2_report();
    runtime_toypad_report();
    runtime_file_report();
    printf("Runtime: memory operations=%zu\n", memory_calls.load(std::memory_order_relaxed));
}

void runtime_finalize(void *dso) {
    /* Mark before invoking: repeated or recursive finalization cannot run twice.
       Restart at the end to include handlers registered by a destructor. */
    for (;;) {
        ExitHandler handler{};
        {
            const std::lock_guard<std::mutex> guard(handler_lock);
            size_t i = handler_count;
            while (i && (!handlers[i - 1].active || (dso && handlers[i - 1].dso != dso))) --i;
            if (!i) return;
            handler = handlers[i - 1];
            handlers[i - 1].active = 0;
        }
        if (handler.with_arg) handler.callback.with_arg(handler.argument);
        else handler.callback.plain();
    }
}

void runtime_libc_register() {
    runtime_register_data_exports(libc_data_exports, sizeof(libc_data_exports) / sizeof(libc_data_exports[0]));
    runtime_register_exports(libc_func_exports, sizeof(libc_func_exports) / sizeof(libc_func_exports[0]));
}

uintptr_t runtime_resolve(const char *name, int is_data) {
    if (!(capabilities & 1)) return 0;
    runtime_registry_init();
    if (!is_data) {
        uintptr_t online = runtime_net_resolve(name);
        if (online) return online;
    }
    uintptr_t target = runtime_registry_resolve(name, is_data);
    if (target) return target;
    if (!is_data) return bbgpu_resolve(name);
    return 0;
}

const char *runtime_import_name(const char *name) {
    const char *symbol = runtime_symbol(name);
    return symbol ? symbol : "name not resolved; see analysis.json import_name_hints";
}

} // extern "C"

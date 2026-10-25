/* Platform abstraction implementation for Linux, Windows, and macOS. */
#define _GNU_SOURCE
#include "platform/platform.h"
#include "platform/time.h"
#include "platform/system.h"
#include "platform/memory.h"
#include "platform/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(BB_PLATFORM_WINDOWS)
#include "win32_compat.h"
#include "win32_memory.h"
#include "host_sync.h"
#include <windows.h>
#include <bcrypt.h>

const char *bb_platform_name(void) { return "Windows"; }

#elif defined(BB_PLATFORM_MACOS)
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>

const char *bb_platform_name(void) { return "macOS"; }

#else /* BB_PLATFORM_LINUX / POSIX */
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <fcntl.h>

const char *bb_platform_name(void) { return "Linux"; }
#endif

const char *bb_platform_arch(void) {
#if defined(BB_ARCH_X86_64)
    return "x86_64";
#elif defined(BB_ARCH_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}

/* --------------------------------------------------------------------------
 * Timing & Clocks
 * -------------------------------------------------------------------------- */

#if defined(BB_PLATFORM_WINDOWS)

static uint64_t win_qpc_frequency(void) {
    static uint64_t freq = 0;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t bb_platform_monotonic_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t f = win_qpc_frequency(), n = (uint64_t)c.QuadPart;
    return (n / f) * 1000000000ULL + (n % f) * 1000000000ULL / f;
}

uint64_t bb_platform_realtime_ns(void) {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    uint64_t ticks = ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
    return (ticks - UINT64_C(116444736000000000)) * 100;
}

void bb_platform_sleep_until_ns(uint64_t deadline_ns) {
    host_sleep_until_ns(deadline_ns);
}

void bb_platform_sleep_ns(uint64_t ns) {
    host_sleep_ns(ns);
}

int64_t bb_platform_utc_offset(time_t when) {
    return runtime_utc_offset(when);
}

#elif defined(BB_PLATFORM_MACOS)

uint64_t bb_platform_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t bb_platform_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void bb_platform_sleep_ns(uint64_t ns) {
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL)
    };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

void bb_platform_sleep_until_ns(uint64_t deadline_ns) {
    while (1) {
        uint64_t now = bb_platform_monotonic_ns();
        if (now >= deadline_ns) break;
        bb_platform_sleep_ns(deadline_ns - now);
    }
}

int64_t bb_platform_utc_offset(time_t when) {
    struct tm tm_val;
    localtime_r(&when, &tm_val);
    return (int64_t)tm_val.tm_gmtoff;
}

#else /* BB_PLATFORM_LINUX / POSIX */

static inline uint64_t linux_read_clock(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t bb_platform_monotonic_ns(void) {
    return linux_read_clock(CLOCK_MONOTONIC);
}

uint64_t bb_platform_realtime_ns(void) {
    return linux_read_clock(CLOCK_REALTIME);
}

void bb_platform_sleep_ns(uint64_t ns) {
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL)
    };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

void bb_platform_sleep_until_ns(uint64_t deadline_ns) {
    struct timespec ts = {
        .tv_sec = (time_t)(deadline_ns / 1000000000ULL),
        .tv_nsec = (long)(deadline_ns % 1000000000ULL)
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) && errno == EINTR) {}
}

int64_t bb_platform_utc_offset(time_t when) {
    struct tm tm_val;
    localtime_r(&when, &tm_val);
    return (int64_t)tm_val.tm_gmtoff;
}

#endif

/* --------------------------------------------------------------------------
 * System & Hardware
 * -------------------------------------------------------------------------- */

#if defined(BB_PLATFORM_WINDOWS)

int bb_platform_random(void *buffer, size_t size) {
    return runtime_random(buffer, size);
}

void bb_platform_set_thread_name(const char *name) {
    runtime_win_set_thread_name(name);
}

void bb_platform_set_guest_tcb(void *tcb) {
    runtime_win_set_tcb(tcb);
}

size_t bb_platform_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
}

int bb_platform_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#elif defined(BB_PLATFORM_MACOS)

int bb_platform_random(void *buffer, size_t size) {
    arc4random_buf(buffer, size);
    return 0;
}

void bb_platform_set_thread_name(const char *name) {
    pthread_setname_np(name);
}

void bb_platform_set_guest_tcb(void *tcb) {
    /* macOS x86-64 thread pointer configuration */
    (void)tcb;
}

size_t bb_platform_page_size(void) {
    long sz = sysconf(_SC_PAGESIZE);
    return sz > 0 ? (size_t)sz : 4096;
}

int bb_platform_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}

#else /* BB_PLATFORM_LINUX / POSIX */

int bb_platform_random(void *buffer, size_t size) {
    ssize_t ret = syscall(SYS_getrandom, buffer, size, 0);
    if (ret == (ssize_t)size) return 0;

    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (char *)buffer + done, size - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

void bb_platform_set_thread_name(const char *name) {
    pthread_setname_np(pthread_self(), name);
}

void bb_platform_set_guest_tcb(void *tcb) {
#if defined(BB_ARCH_X86_64) && defined(SYS_arch_prctl)
    enum { ARCH_SET_FS = 0x1002 };
    syscall(SYS_arch_prctl, ARCH_SET_FS, (unsigned long)tcb);
#else
    (void)tcb;
#endif
}

size_t bb_platform_page_size(void) {
    long sz = sysconf(_SC_PAGESIZE);
    return sz > 0 ? (size_t)sz : 4096;
}

int bb_platform_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}

#endif

/* --------------------------------------------------------------------------
 * Memory Management
 * -------------------------------------------------------------------------- */

#if defined(BB_PLATFORM_WINDOWS)

void *bb_platform_reserve_space(uintptr_t base, size_t size) {
    if (win_mem_space(base, base + size) != 0) return NULL;
    return (void *)base;
}

void *bb_platform_alloc_committed(uintptr_t base, size_t size, int prot) {
    (void)prot;
    return win_mem_private(base, base + size);
}

int bb_platform_protect(void *addr, size_t size, int prot) {
    return win_mem_protect((uintptr_t)addr, (uintptr_t)addr + size, prot);
}

int bb_platform_release(void *addr, size_t size) {
    return win_mem_release((uintptr_t)addr, (uintptr_t)addr + size, NULL);
}

#elif defined(BB_PLATFORM_MACOS)

static int macos_translate_prot(int prot) {
    int p = PROT_NONE;
    if (prot & BB_PROT_READ) p |= PROT_READ;
    if (prot & BB_PROT_WRITE) p |= PROT_WRITE;
    if (prot & BB_PROT_EXEC) p |= PROT_EXEC;
    return p;
}

void *bb_platform_reserve_space(uintptr_t base, size_t size) {
    void *ptr = mmap((void *)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

void *bb_platform_alloc_committed(uintptr_t base, size_t size, int prot) {
    int p = macos_translate_prot(prot);
    int flags = MAP_PRIVATE | MAP_ANON;
    if (base != 0) {
        flags |= MAP_FIXED;
    }
    void *ptr = mmap((void *)base, size, p, flags, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

int bb_platform_protect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, macos_translate_prot(prot));
}

int bb_platform_release(void *addr, size_t size) {
    void *ptr = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return (ptr == MAP_FAILED) ? -1 : 0;
}

#else /* BB_PLATFORM_LINUX / POSIX */

static int linux_translate_prot(int prot) {
    int p = PROT_NONE;
    if (prot & BB_PROT_READ) p |= PROT_READ;
    if (prot & BB_PROT_WRITE) p |= PROT_WRITE;
    if (prot & BB_PROT_EXEC) p |= PROT_EXEC;
    return p;
}

void *bb_platform_reserve_space(uintptr_t base, size_t size) {
    void *ptr = mmap((void *)base, size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

void *bb_platform_alloc_committed(uintptr_t base, size_t size, int prot) {
    int p = linux_translate_prot(prot);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (base != 0) {
        flags |= MAP_FIXED;
    }
    void *ptr = mmap((void *)base, size, p, flags, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

int bb_platform_protect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, linux_translate_prot(prot));
}

int bb_platform_release(void *addr, size_t size) {
    void *ptr = mmap(addr, size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return (ptr == MAP_FAILED) ? -1 : 0;
}

#endif

/* --------------------------------------------------------------------------
 * Threading
 * -------------------------------------------------------------------------- */

int bb_platform_thread_start(BbPlatformThread *thread, size_t stack, void *(*entry)(void *), void *argument) {
#if defined(BB_PLATFORM_WINDOWS)
    return host_thread_start((HostThread *)thread, stack, entry, argument);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) {
        pthread_attr_setstacksize(&attr, stack);
    }
    int err = pthread_create(thread, &attr, entry, argument);
    pthread_attr_destroy(&attr);
    return err;
#endif
}

int bb_platform_thread_join(BbPlatformThread thread) {
#if defined(BB_PLATFORM_WINDOWS)
    return host_thread_join((HostThread)thread);
#else
    return pthread_join(thread, NULL);
#endif
}

void bb_platform_thread_detach(BbPlatformThread thread) {
#if defined(BB_PLATFORM_WINDOWS)
    host_thread_detach((HostThread)thread);
#else
    pthread_detach(thread);
#endif
}

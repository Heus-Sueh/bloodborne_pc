#include "platform/platform.h"
#include "platform/time.h"
#include "platform/system.h"
#include "platform/memory.h"
#include "platform/sync.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>

static void *test_worker(void *arg) {
    int *val = static_cast<int *>(arg);
    *val = 42;
    bb_platform_set_thread_name("bb_plat_test");
    return nullptr;
}

int main() {
    std::cout << "[PLATFORM C++23 TEST] Initializing...\n";

    // 1. Identification
    const char *pname = bb_platform_name();
    const char *arch = bb_platform_arch();
    assert(pname != nullptr && std::strlen(pname) > 0);
    assert(arch != nullptr && std::strlen(arch) > 0);
    std::cout << "Platform: " << pname << " (" << arch << ")\n";

    // 2. System properties
    size_t page_sz = bb_platform_page_size();
    int cpu_count = bb_platform_cpu_count();
    assert(page_sz >= 4096);
    assert(cpu_count >= 1);
    std::cout << "Page size: " << page_sz << " bytes, CPU count: " << cpu_count << '\n';

    // 3. Random generation
    uint8_t rand_buf1[32] = {0};
    uint8_t rand_buf2[32] = {0};
    int r1 = bb_platform_random(rand_buf1, sizeof(rand_buf1));
    int r2 = bb_platform_random(rand_buf2, sizeof(rand_buf2));
    assert(r1 == 0 && r2 == 0);
    assert(std::memcmp(rand_buf1, rand_buf2, sizeof(rand_buf1)) != 0);

    // 4. Clocks and sleep
    uint64_t m1 = bb_platform_monotonic_ns();
    uint64_t r_time = bb_platform_realtime_ns();
    assert(r_time > 1700000000ULL * 1000000000ULL); // Beyond Nov 2023

    bb_platform_sleep_ns(10000000ULL); // 10 ms
    uint64_t m2 = bb_platform_monotonic_ns();
    assert(m2 > m1);
    assert(m2 - m1 >= 8000000ULL);

    // 5. Synchronization & Threading
    BbPlatformMutex mutex;
    bb_platform_mutex_init(&mutex);
    bb_platform_mutex_lock(&mutex);
    bb_platform_mutex_unlock(&mutex);

    BbPlatformRwlock rwlock;
    bb_platform_rwlock_init(&rwlock);
    bb_platform_rwlock_write_lock(&rwlock);
    bb_platform_rwlock_write_unlock(&rwlock);
    bb_platform_rwlock_read_lock(&rwlock);
    bb_platform_rwlock_read_unlock(&rwlock);

    int worker_val = 0;
    BbPlatformThread thread;
    int err = bb_platform_thread_start(&thread, 0, test_worker, &worker_val);
    assert(err == 0);
    err = bb_platform_thread_join(thread);
    assert(err == 0);
    assert(worker_val == 42);

    // 6. Virtual Memory
    void *mem = bb_platform_alloc_committed(0, page_sz, BB_PROT_READ | BB_PROT_WRITE);
    assert(mem != nullptr);
    std::memset(mem, 0xAB, page_sz);
    assert(*static_cast<uint8_t *>(mem) == 0xAB);

    int prot_err = bb_platform_protect(mem, page_sz, BB_PROT_READ);
    assert(prot_err == 0);

    int rel_err = bb_platform_release(mem, page_sz);
    assert(rel_err == 0);

    std::cout << "PASS: Platform abstraction layer C++23 tests succeeded!\n";
    return 0;
}

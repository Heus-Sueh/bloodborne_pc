#include "runtime_rewatch.hpp"

#ifdef _WIN32
extern "C" {
void runtime_re_watch_now(uint64_t address) {
    (void)address;
}
void runtime_re_watch_report(void) {}
void runtime_re_watch(uint64_t address) {
    (void)address;
}
void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) {
    (void)pad_buffer;
    (void)reads;
}
}
#else

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/bbhooks.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr size_t kWatchSlots = 256;
constexpr size_t kWatchRingPages = 8;
constexpr size_t kMaxWatchRings = 512;

std::array<uint64_t, kWatchSlots> s_watch_rip{};
std::array<uint64_t, kWatchSlots> s_watch_count{};
uint64_t s_watch_samples = 0;
// per address: rdx rsi rdi rbp rsp, 8 stack qwords
std::array<std::array<uint64_t, 13>, kWatchSlots> s_watch_last{};

std::array<struct perf_event_mmap_page *, kMaxWatchRings> s_watch_rings{};
std::array<int, kMaxWatchRings> s_watch_fds{};
int s_watch_ring_count = 0;
uint64_t s_watch_address = 0;
uint64_t s_watch_follow_to = 0;
int s_watch_started = 0;
uint64_t s_watch_requested = 0;

void WatchNote(uint64_t rip, const uint64_t *last) {
    ++s_watch_samples;
    for (size_t i = 0; i < kWatchSlots; ++i) {
        const size_t k = (static_cast<size_t>((rip * 0x9E3779B97F4A7C15ull) >> 56) + i) & (kWatchSlots - 1);
        if (s_watch_rip[k] == rip || s_watch_rip[k] == 0) {
            s_watch_rip[k] = rip;
            ++s_watch_count[k];
            std::memcpy(s_watch_last[k].data(), last, sizeof(uint64_t) * 13);

            static int follow = -1;
            if (follow < 0) {
                const char *e = std::getenv("BB_RE_WATCH_FOLLOW");
                follow = e ? std::atoi(e) : 0;
            }
            if (follow && (rip >> 40) && !s_watch_follow_to && last[1] != last[2]) {
                s_watch_follow_to = s_watch_address + (last[2] - last[1]);
            }
            return;
        }
    }
}

void WatchStop() {
    for (int r = 0; r < s_watch_ring_count; ++r) {
        munmap(s_watch_rings[r], (1 + kWatchRingPages) * 4096);
        close(s_watch_fds[r]);
    }
    s_watch_ring_count = 0;
    s_watch_rip.fill(0);
    s_watch_count.fill(0);
}

void WatchStart(uint64_t address) {
    s_watch_address = address;
    struct perf_event_attr a{};
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof(a);
    a.bp_type = HW_BREAKPOINT_RW;
    a.bp_addr = address;
    a.bp_len = HW_BREAKPOINT_LEN_4;
    a.sample_period = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
    a.sample_regs_user = (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 7); // dx si di bp sp
    a.sample_stack_user = 64;

    DIR *dir = opendir("/proc/self/task");
    int threads = 0;
    for (struct dirent *e = nullptr; dir && (e = readdir(dir));) {
        if (e->d_name[0] == '.') {
            continue;
        }
        ++threads;
        const int fd =
            static_cast<int>(syscall(SYS_perf_event_open, &a, static_cast<pid_t>(std::atoi(e->d_name)), -1, -1, 0));
        if (fd < 0 || s_watch_ring_count >= static_cast<int>(kMaxWatchRings)) {
            continue;
        }
        void *ring = mmap(nullptr, (1 + kWatchRingPages) * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ring != MAP_FAILED) {
            s_watch_fds[s_watch_ring_count] = fd;
            s_watch_rings[s_watch_ring_count++] = static_cast<struct perf_event_mmap_page *>(ring);
        } else {
            close(fd);
        }
    }
    if (dir) {
        closedir(dir);
    }
    std::printf("RE watch: %#llx on %d of %d threads\n", static_cast<unsigned long long>(address),
                s_watch_ring_count, threads);
}

void WatchDrain(int force) {
    for (int r = 0; r < s_watch_ring_count; ++r) {
        struct perf_event_mmap_page *page = s_watch_rings[r];
        const uint8_t *data = reinterpret_cast<const uint8_t *>(page) + 4096;
        constexpr uint64_t size = kWatchRingPages * 4096;
        const uint64_t head = __atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE);
        uint64_t tail = page->data_tail;

        while (tail < head) {
            struct perf_event_header h{};
            for (size_t i = 0; i < sizeof(h); ++i) {
                reinterpret_cast<uint8_t *>(&h)[i] = data[(tail + i) % size];
            }
            if (h.size == 0) {
                break;
            }
            if (h.type == PERF_RECORD_SAMPLE) {
                uint64_t w[17] = {0};
                for (size_t j = 0; j < 17; ++j) {
                    for (size_t i = 0; i < 8; ++i) {
                        reinterpret_cast<uint8_t *>(&w[j])[i] = data[(tail + sizeof(h) + 8 * j + i) % size];
                    }
                }
                const uint64_t last[13] = {w[2], w[3], w[4],  w[5],  w[6],  w[8], w[9],
                                           w[10], w[11], w[12], w[13], w[14], w[15]};
                WatchNote(w[0], last);
            }
            tail += h.size;
        }
        __atomic_store_n(&page->data_tail, head, __ATOMIC_RELEASE);
    }

    if (s_watch_follow_to && s_watch_follow_to != s_watch_address) {
        static int followed = 0;
        static int hops = -1;
        if (hops < 0) {
            const char *e = std::getenv("BB_RE_WATCH_FOLLOW");
            hops = e ? std::atoi(e) : 0;
        }
        if (followed++ < hops) {
            std::printf("RE watch: copied from %#llx to %#llx: watching there\n",
                        static_cast<unsigned long long>(s_watch_address),
                        static_cast<unsigned long long>(s_watch_follow_to));
            const uint64_t to = s_watch_follow_to;
            WatchStop();
            WatchStart(to);
            s_watch_follow_to = 0;
            return;
        }
    }

    static std::time_t printed = 0;
    const std::time_t now = std::time(nullptr);
    if (!force && now - printed < 5) {
        return;
    }
    printed = now;

    std::printf("RE watch %#llx: %llu samples; by the instruction after the access:",
                static_cast<unsigned long long>(s_watch_address),
                static_cast<unsigned long long>(s_watch_samples));
    for (size_t i = 0; i < kWatchSlots; ++i) {
        if (s_watch_rip[i]) {
            std::printf(" %#llx:%llu", static_cast<unsigned long long>(s_watch_rip[i]),
                        static_cast<unsigned long long>(s_watch_count[i]));
            s_watch_count[i] = 0;
            if (s_watch_rip[i] >> 40) {
                std::printf(" [rdx %#llx rsi %#llx rdi %#llx rbp %#llx rsp %#llx; stack",
                            static_cast<unsigned long long>(s_watch_last[i][0]),
                            static_cast<unsigned long long>(s_watch_last[i][1]),
                            static_cast<unsigned long long>(s_watch_last[i][2]),
                            static_cast<unsigned long long>(s_watch_last[i][3]),
                            static_cast<unsigned long long>(s_watch_last[i][4]));
                for (size_t j = 5; j < 13; ++j) {
                    std::printf(" %#llx", static_cast<unsigned long long>(s_watch_last[i][j]));
                }
                std::printf("]");
            }
        }
    }
    std::printf("\n");
}

} // namespace

extern "C" {

void runtime_re_watch_now(uint64_t address) {
    if (!s_watch_started) {
        s_watch_started = 1;
        WatchStart(address);
    }
}

void runtime_re_watch_report(void) {
    if (s_watch_started) {
        WatchDrain(1);
    }
}

void runtime_re_watch(uint64_t address) {
    if (!s_watch_requested) {
        s_watch_requested = address;
    }
}

void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) {
    static int camstate = -1;
    if (camstate < 0) {
        const char *e = std::getenv("BB_RE_WATCH_CAMSTATE");
        camstate = (e && e[0] == '1') ? 1 : 0;
    }
    if (camstate == 1 && !s_watch_requested && bbhook_camera_state()) {
        runtime_re_watch(bbhook_camera_state() + 0x54);
    }

    static long watch_at = -1;
    if (watch_at < 0) {
        const char *e = std::getenv("BB_RE_WATCH_AT");
        watch_at = e ? std::atol(e) : 3000;
    }
    if (s_watch_requested && !s_watch_started && static_cast<long>(reads) >= watch_at) {
        s_watch_started = 1;
        WatchStart(s_watch_requested);
    }
    if (s_watch_started) {
        WatchDrain(0);
        return;
    }

    static const char *watch_env = nullptr;
    static const char *watch_addr = nullptr;
    static int watch_checked = 0;
    if (!watch_checked) {
        watch_env = std::getenv("BB_RE_WATCH");
        watch_addr = std::getenv("BB_RE_WATCH_ADDR");
        watch_checked = 1;
    }
    if (!watch_env && !watch_addr) {
        return;
    }
    if (static_cast<long>(reads) == watch_at) {
        const uint64_t target_addr = watch_addr
                                         ? std::strtoull(watch_addr, nullptr, 0)
                                         : reinterpret_cast<uint64_t>(pad_buffer) +
                                               std::strtoull(watch_env, nullptr, 0);
        WatchStart(target_addr);
        s_watch_started = 1;
    }
}

} // extern "C"

#endif

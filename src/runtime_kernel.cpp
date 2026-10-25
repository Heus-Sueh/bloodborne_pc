#include "runtime_kernel.hpp"
#include "runtime.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <x86intrin.h>

#define ERR(n) ORBIS_ERR(n)
#define PAGE 16384

namespace bb::runtime {

struct GuestTimespec {
    int64_t sec{0};
    int64_t nsec{0};
};

struct GuestTimeval {
    int64_t sec{0};
    int64_t usec{0};
};

struct GuestTimezone {
    int32_t minuteswest{0};
    int32_t dsttime{0};
};

struct GuestSigset {
    uint32_t bits[4]{0};
};

struct GuestRusage {
    GuestTimeval utime;
    GuestTimeval stime;
    int64_t rest[14]{0};
};

using KeyDestructor = void (ABI *)(void *);

struct KeySlot {
    bool used{false};
    KeyDestructor destructor{nullptr};
};

constexpr size_t KEYS_COUNT = 256;

static inline int32_t FailPosix(int e) {
    *runtime_errno() = runtime_guest_errno(e);
    return -1;
}

class KernelService {
public:
    static KernelService& Instance() {
        static KernelService instance;
        return instance;
    }

    KernelService() {
        clock_gettime(CLOCK_MONOTONIC, &process_start_);
    }

    static bool HostClock(uint32_t id, clockid_t& out) {
        switch (id) {
            case 0:
            case 9:
            case 10:
                out = CLOCK_REALTIME;
                return true;
            case 4:
            case 11:
            case 12:
            case 5:
            case 7:
            case 8:
                out = CLOCK_MONOTONIC;
                return true;
            case 13:
                out = CLOCK_REALTIME_COARSE;
                return true;
            case 14:
                out = CLOCK_THREAD_CPUTIME_ID;
                return true;
            case 2:
            case 15:
                out = CLOCK_PROCESS_CPUTIME_ID;
                return true;
            case 16:
            case 17:
            case 18:
            case 19:
                out = CLOCK_MONOTONIC;
                return true;
            default:
                return false;
        }
    }

    static int ClockRead(uint32_t id, GuestTimespec* ts) {
        clockid_t host{};
        if (!ts || !HostClock(id, host)) return EINVAL;
        struct timespec t;
        if (clock_gettime(host, &t)) return errno;
        if (id == 13) t.tv_nsec = 0;
        ts->sec = t.tv_sec;
        ts->nsec = t.tv_nsec;
        return 0;
    }

    uint64_t ProcessTime() const {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return static_cast<uint64_t>(t.tv_sec - process_start_.tv_sec) * 1000000ULL +
               static_cast<uint64_t>((t.tv_nsec - process_start_.tv_nsec) / 1000);
    }

    uint64_t ProcessTimeCounter() const {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return static_cast<uint64_t>(t.tv_sec - process_start_.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(t.tv_nsec - process_start_.tv_nsec);
    }

    uint64_t TscFrequency() {
        uint64_t hz = tsc_hz_.load(std::memory_order_relaxed);
        if (hz == 0) {
            struct timespec a{}, b{}, nap{0, 20000000};
            clock_gettime(CLOCK_MONOTONIC, &a);
            uint64_t t0 = __rdtsc();
            nanosleep(&nap, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &b);
            uint64_t t1 = __rdtsc();
            uint64_t ns = static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ULL +
                          static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
            hz = (t1 - t0) * 1000000000ULL / (ns ? ns : 1ULL);
            tsc_hz_.store(hz, std::memory_order_relaxed);
        }
        return hz;
    }

    int SleepNs(uint64_t ns) {
        struct timespec t{
            .tv_sec = static_cast<time_t>(ns / 1000000000ULL),
            .tv_nsec = static_cast<long>(ns % 1000000000ULL)
        };
        struct timespec a{}, b{};
        clock_gettime(CLOCK_MONOTONIC, &a);
        int result = 0;
        while (nanosleep(&t, &t)) {
            if (errno != EINTR) {
                result = errno;
                break;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        const uint64_t elapsed = static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ULL +
                                 static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
        sleep_calls_.fetch_add(1, std::memory_order_relaxed);
        sleep_total_ns_.fetch_add(elapsed, std::memory_order_relaxed);
        runtime_wait_note(3, elapsed);
        return result;
    }

    void SleepStats(uint64_t* calls, uint64_t* ns) {
        if (calls) *calls = sleep_calls_.exchange(0, std::memory_order_relaxed);
        if (ns) *ns = sleep_total_ns_.exchange(0, std::memory_order_relaxed);
    }

    int32_t KeyCreate(uint32_t* key, KeyDestructor destructor) {
        if (!key) return ERR(22);
        std::lock_guard<std::mutex> lock(key_lock_);
        for (uint32_t i = 1; i < KEYS_COUNT; ++i) {
            if (!keys_[i].used) {
                keys_[i].used = true;
                keys_[i].destructor = destructor;
                *key = i;
                return 0;
            }
        }
        return ERR(35);
    }

    int32_t KeyDelete(uint32_t key) {
        if (!key || key >= KEYS_COUNT) return ERR(22);
        std::lock_guard<std::mutex> lock(key_lock_);
        int32_t r = keys_[key].used ? 0 : ERR(22);
        keys_[key].used = false;
        keys_[key].destructor = nullptr;
        return r;
    }

    KeyDestructor GetKeyDestructor(uint32_t key) {
        std::lock_guard<std::mutex> lock(key_lock_);
        if (key < KEYS_COUNT && keys_[key].used) {
            return keys_[key].destructor;
        }
        return nullptr;
    }

    bool IsKeyUsed(uint32_t key) {
        std::lock_guard<std::mutex> lock(key_lock_);
        return (key < KEYS_COUNT && keys_[key].used);
    }

private:
    struct timespec process_start_{};
    std::atomic<uint64_t> tsc_hz_{0};
    std::atomic<uint64_t> sleep_calls_{0};
    std::atomic<uint64_t> sleep_total_ns_{0};

    std::mutex key_lock_;
    std::array<KeySlot, KEYS_COUNT> keys_{};
};

thread_local std::array<void*, KEYS_COUNT> t_key_values{};
thread_local GuestSigset t_signal_mask{};
static uintptr_t g_signal_handlers[128]{};

} // namespace bb::runtime

// Wait profiling
struct WaitSite {
    std::atomic<uint64_t> key{0};
    uint64_t site{0};
    uint64_t site2{0};
    uint64_t site3{0};
    int tid{0};
    int kind{0};
    char name[16]{};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> ns{0};
};

static WaitSite g_wait_sites[512]{};
static thread_local int t_wait_tid = 0;

static void guest_call_sites(uint64_t out[3]) {
    constexpr uintptr_t text_lo = 0x800000000ULL;
    constexpr uintptr_t text_hi = 0x800000000ULL + 0x50d96dcULL;
    static thread_local uintptr_t stack_hi = 0;
    if (!stack_hi) {
#ifdef _WIN32
        uintptr_t low = 0;
        bb_thread_stack(&low, &stack_hi);
#else
        pthread_attr_t attr;
        void* base = nullptr;
        size_t size = 0;
        if (!pthread_getattr_np(pthread_self(), &attr)) {
            pthread_attr_getstack(&attr, &base, &size);
            pthread_attr_destroy(&attr);
        }
        stack_hi = reinterpret_cast<uintptr_t>(base) + size;
#endif
    }
    const auto* sp = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    int found = 0;
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 400 && found < 3 && reinterpret_cast<uintptr_t>(sp + i + 1) <= stack_hi; ++i) {
        if (sp[i] >= text_lo && sp[i] < text_hi) {
            out[found++] = sp[i] - text_lo;
        }
    }
}

// CPU Sampling
constexpr size_t SAMPLE_SLOTS = 8192;
static std::atomic<uint64_t> g_sample_keys[SAMPLE_SLOTS]{};
static std::atomic<uint64_t> g_sample_counts[SAMPLE_SLOTS]{};
static uint64_t g_sample_rips[SAMPLE_SLOTS]{};
static uint64_t g_sample_callers[SAMPLE_SLOTS]{};
static std::atomic<uint64_t> g_samples_total{0};
static uint64_t g_gpu_text_lo = 0;
static uint64_t g_gpu_text_hi = 0;

static void sample_handler(int /*sig*/, siginfo_t* /*info*/, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    uint64_t rip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    constexpr uint64_t text_lo = 0x800000000ULL;
    constexpr uint64_t text_hi = 0x800000000ULL + 0x50d96dcULL;

    uint64_t caller = 0;
    if (rip >= text_lo && rip < text_hi) {
        uint64_t frame[2] = {0};
        struct iovec local = {frame, sizeof(frame)};
        struct iovec remote = {reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RBP]), sizeof(frame)};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(sizeof(frame)) &&
            frame[1] >= text_lo && frame[1] < text_hi) {
            caller = frame[1] - text_lo;
        }
    } else {
        uint64_t stack[256] = {0};
        struct iovec local = {stack, sizeof(stack)};
        struct iovec remote = {reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RSP]), sizeof(stack)};
        const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        for (ssize_t i = 0; i < got / 8; ++i) {
            if (stack[i] >= text_lo && stack[i] < text_hi) {
                caller = stack[i] - text_lo;
                break;
            }
        }
        if (!caller && g_gpu_text_hi && (rip < g_gpu_text_lo || rip >= g_gpu_text_hi)) {
            for (ssize_t i = 0; i < got / 8; ++i) {
                if (stack[i] >= g_gpu_text_lo && stack[i] < g_gpu_text_hi) {
                    caller = stack[i] | (1ULL << 63);
                    break;
                }
            }
        }
    }

    const uint64_t key = (rip * 0x9E3779B97F4A7C15ULL) ^ caller ^ 1ULL;
    g_samples_total.fetch_add(1, std::memory_order_relaxed);

    for (uint64_t i = 0, slot = (key * 0x9E3779B97F4A7C15ULL) >> 51; i < SAMPLE_SLOTS; ++i) {
        auto* k = &g_sample_keys[(slot + i) % SAMPLE_SLOTS];
        uint64_t expected = 0;
        if (k->load(std::memory_order_relaxed) == key ||
            k->compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
            g_sample_rips[(slot + i) % SAMPLE_SLOTS] = rip;
            g_sample_callers[(slot + i) % SAMPLE_SLOTS] = caller;
            g_sample_counts[(slot + i) % SAMPLE_SLOTS].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

static void* sampler_main(void* arg) {
    const auto* name = static_cast<const char*>(arg);
    pid_t tid = (!std::strcmp(name, "main")) ? getpid() : 0;
    while (!tid) {
        DIR* dir = opendir("/proc/self/task");
        struct dirent* e = nullptr;
        while (dir && (e = readdir(dir))) {
            char path[300]{};
            char comm[32]{};
            std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            if (std::fgets(comm, sizeof(comm), f) && !std::strncmp(comm, name, std::strlen(name))) {
                tid = static_cast<pid_t>(std::atoi(e->d_name));
            }
            std::fclose(f);
        }
        if (dir) closedir(dir);
        if (!tid) sleep(1);
    }
    std::printf("Runtime: sampling thread %s (%d)\n", name, static_cast<int>(tid));
    if (tid == getpid() || std::strncmp(name, "bb:", 3)) {
        std::printf("Runtime: warning: sampling a game thread disturbs it (diagnostics only)\n");
    }
    for (;;) {
        if (syscall(SYS_tgkill, getpid(), tid, SIGPROF)) break;
        struct timespec t = {0, 500000};
        nanosleep(&t, nullptr);
    }
    return nullptr;
}

static void sample_report();

static void sample_start() {
    static bool started = false;
    const char* name = std::getenv("BB_SAMPLE_THREAD");
    if (started || !name || !*name) return;
    started = true;

    struct sigaction action{};
    action.sa_sigaction = sample_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);

    FILE* maps = std::fopen("/proc/self/maps", "r");
    char line[512]{};
    while (maps && std::fgets(line, sizeof(line), maps)) {
        unsigned long lo = 0, hi = 0;
        char perms[8]{};
        if (std::strstr(line, "libbbgpu.so") &&
            std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && perms[2] == 'x') {
            g_gpu_text_lo = lo;
            g_gpu_text_hi = hi;
            break;
        }
    }
    if (maps) std::fclose(maps);

    pthread_t thread;
    pthread_create(&thread, nullptr, sampler_main, const_cast<char*>(name));
    pthread_detach(thread);
}

static void* sample_reporter(void* arg) {
    const unsigned seconds = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
    for (;;) {
        struct timespec left{static_cast<time_t>(seconds), 0};
        while (nanosleep(&left, &left) != 0 && errno == EINTR) {}
        sample_report();
        std::fflush(stdout);
    }
    return nullptr;
}

__attribute__((constructor)) static void sample_report_timer() {
    const char* v = std::getenv("BB_SAMPLE_REPORT_SECONDS");
    const unsigned seconds = v ? static_cast<unsigned>(std::atoi(v)) : 0;
    if (!seconds || !std::getenv("BB_SAMPLE_THREAD")) return;
    sample_start();
    pthread_t thread;
    pthread_create(&thread, nullptr, sample_reporter, reinterpret_cast<void*>(static_cast<uintptr_t>(seconds)));
    pthread_detach(thread);
}

static void sample_report() {
    static bool got_dumped = false;
    if (!got_dumped) {
        got_dumped = true;
        if (const char* got = std::getenv("BB_DUMP_GOT"); got && *got) {
            const uint64_t target = *reinterpret_cast<const uint64_t*>(0x800000000ULL + std::strtoull(got, nullptr, 0));
            Dl_info dl{};
            dladdr(reinterpret_cast<void*>(target), &dl);
            std::printf("Runtime: GOT %s -> %#lx %s:%s+%#lx\n", got,
                        static_cast<unsigned long>(target),
                        dl.dli_fname ? dl.dli_fname : "?",
                        dl.dli_sname ? dl.dli_sname : "?",
                        static_cast<unsigned long>(target - reinterpret_cast<uint64_t>(dl.dli_sname ? dl.dli_saddr : dl.dli_fbase)));
        }
    }

    const uint64_t total = g_samples_total.exchange(0, std::memory_order_relaxed);
    if (!total) return;

    struct SampleRow {
        uint64_t rip;
        uint64_t caller;
        uint64_t count;
    };
    static std::array<SampleRow, SAMPLE_SLOTS> rows{};
    int n = 0;

    for (size_t i = 0; i < SAMPLE_SLOTS; ++i) {
        uint64_t c = g_sample_counts[i].exchange(0, std::memory_order_relaxed);
        if (c) {
            rows[n++] = SampleRow{g_sample_rips[i], g_sample_callers[i], c};
        }
    }

    for (int i = 1; i < n; ++i) {
        for (int j = i; j > 0 && rows[j].count > rows[j - 1].count; --j) {
            std::swap(rows[j], rows[j - 1]);
        }
    }

    static int all = -1;
    if (all < 0) {
        const char* v = std::getenv("BB_SAMPLE_ALL");
        all = (v && v[0] == '1');
    }
    if (all) {
        for (int i = 0; i < n; ++i) {
            Dl_info dl{};
            dladdr(reinterpret_cast<void*>(rows[i].rip), &dl);
            const char* module = dl.dli_fname ? std::strrchr(dl.dli_fname, '/') : nullptr;
            char from[96] = "";
            if (rows[i].caller && !(rows[i].caller >> 63)) {
                std::snprintf(from, sizeof(from), " from game+%#lx", static_cast<unsigned long>(rows[i].caller));
            } else if (rows[i].caller >> 63) {
                const uint64_t c = rows[i].caller & ~(1ULL << 63);
                Dl_info cl{};
                dladdr(reinterpret_cast<void*>(c), &cl);
                std::snprintf(from, sizeof(from), " from %#lx",
                              static_cast<unsigned long>(c - reinterpret_cast<uint64_t>(cl.dli_fbase)));
            }
            std::printf("Sample: %s+%#lx %llu%s\n", module ? module + 1 : "?",
                        static_cast<unsigned long>(rows[i].rip - reinterpret_cast<uint64_t>(dl.dli_fbase)),
                        static_cast<unsigned long long>(rows[i].count), from);
        }
    }

    std::printf("Thread samples (%llu):", static_cast<unsigned long long>(total));
    for (int i = 0; i < n && i < 16; ++i) {
        constexpr uint64_t text_lo = 0x800000000ULL;
        constexpr uint64_t text_hi = 0x800000000ULL + 0x50d96dcULL;
        if (rows[i].rip >= text_lo && rows[i].rip < text_hi) {
            std::printf("%s +%#lx (from +%#lx) %.1f%%", i ? ";" : "",
                        static_cast<unsigned long>(rows[i].rip - text_lo),
                        static_cast<unsigned long>(rows[i].caller),
                        100.0 * rows[i].count / total);
            continue;
        }
        Dl_info dl{};
        dladdr(reinterpret_cast<void*>(rows[i].rip), &dl);
        const char* module = dl.dli_fname ? std::strrchr(dl.dli_fname, '/') : nullptr;
        std::printf("%s host:%s%s%s+%#lx (from +%#lx) %.1f%%", i ? ";" : "",
                    module ? module + 1 : "?", dl.dli_sname ? ":" : "",
                    dl.dli_sname ? dl.dli_sname : "",
                    static_cast<unsigned long>(rows[i].rip - reinterpret_cast<uint64_t>(dl.dli_sname ? dl.dli_saddr : dl.dli_fbase)),
                    static_cast<unsigned long>(rows[i].caller),
                    100.0 * rows[i].count / total);
    }
    std::printf("\n");
}

// Guest ABI Trampolines
extern "C" {

using namespace bb::runtime;

static ABI int32_t kernel_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = KernelService::ClockRead(id, ts);
    return e ? ERR(runtime_guest_errno(e)) : 0;
}

static ABI int32_t posix_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = KernelService::ClockRead(id, ts);
    return e ? FailPosix(e) : 0;
}

static ABI int32_t posix_clock_getres(uint32_t id, GuestTimespec *ts) {
    clockid_t host{};
    struct timespec t{};
    if (!KernelService::HostClock(id, host)) return FailPosix(EINVAL);
    if (clock_getres(host, &t)) return FailPosix(errno);
    if (ts) {
        ts->sec = t.tv_sec;
        ts->nsec = t.tv_nsec;
    }
    return 0;
}

static ABI uint64_t process_time(void) {
    return KernelService::Instance().ProcessTime();
}

static ABI uint64_t process_time_counter(void) {
    return KernelService::Instance().ProcessTimeCounter();
}

static ABI uint64_t process_time_frequency(void) {
    return 1000000000ULL;
}

static ABI uint64_t read_tsc(void) {
    return __rdtsc();
}

static ABI uint64_t tsc_frequency(void) {
    return KernelService::Instance().TscFrequency();
}

uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }

void runtime_sleep_stats(uint64_t *calls, uint64_t *ns) {
    KernelService::Instance().SleepStats(calls, ns);
}

static ABI int32_t kernel_usleep(uint32_t usec) {
    KernelService::Instance().SleepNs(static_cast<uint64_t>(usec) * 1000ULL);
    return 0;
}

static ABI int32_t posix_usleep(uint32_t usec) {
    KernelService::Instance().SleepNs(static_cast<uint64_t>(usec) * 1000ULL);
    return 0;
}

static ABI uint32_t posix_sleep(uint32_t seconds) {
    KernelService::Instance().SleepNs(static_cast<uint64_t>(seconds) * 1000000000ULL);
    return 0;
}

static ABI int32_t posix_nanosleep(const GuestTimespec *rq, GuestTimespec *rem) {
    if (!rq || rq->nsec < 0 || rq->nsec >= 1000000000 || rq->sec < 0) return FailPosix(EINVAL);
    KernelService::Instance().SleepNs(static_cast<uint64_t>(rq->sec) * 1000000000ULL + static_cast<uint64_t>(rq->nsec));
    if (rem) {
        rem->sec = 0;
        rem->nsec = 0;
    }
    return 0;
}

static ABI int32_t kernel_nanosleep(const GuestTimespec *rq, GuestTimespec *rem) {
    return posix_nanosleep(rq, rem) ? ERR(*runtime_errno()) : 0;
}

static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    tz->minuteswest = static_cast<int32_t>(-RUNTIME_TM_GMTOFF(local) / 60);
    tz->dsttime = 0;
    return 0;
}

static ABI int32_t posix_gettimeofday(GuestTimeval *tv, GuestTimezone *tz) {
    struct timeval t{};
    gettimeofday(&t, nullptr);
    if (tv) {
        tv->sec = t.tv_sec;
        tv->usec = t.tv_usec;
    }
    if (tz) kernel_gettimezone(tz);
    return 0;
}

static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv, nullptr);
}

static ABI int64_t posix_time(int64_t *out) {
    int64_t t = static_cast<int64_t>(time(nullptr));
    if (out) *out = t;
    return t;
}

static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }

static ABI __attribute__((noreturn)) void hard_exit(int status) {
    std::printf("Runtime: guest requested _exit(%d)\n", status);
    runtime_report();
    std::fflush(stdout);
    _exit(status);
}

static ABI void raise_exception(uint32_t code, uint64_t argument) {
    {
        constexpr uint64_t base = 0x800000000ULL;
        constexpr uint64_t hi = base + 0x50d96dcULL;
        const auto* frame = static_cast<const uint64_t*>(__builtin_frame_address(0));
        uint64_t at = 0;
        for (int i = 0; i < 64 && !at; ++i) {
            if (frame[i] >= base && frame[i] < hi) {
                at = frame[i] - base;
            }
        }
        static int stop = -1;
        static int told = 0;
        if (stop < 0) {
            const char* e = std::getenv("BB_GAME_ASSERT_STOP");
            stop = (e && e[0] == '1');
        }
        if (!stop && code == 0xa0020007U && ((at >= 0xbbc3e0 && at < 0xbbc700) || (at >= 0xc33000 && at < 0xc34200))) {
            if (told++ < 5) {
                std::printf("Runtime: the game's blur-kernel assertion at +0x%llx ignored (BB_GAME_ASSERT_STOP=1: stop)\n",
                            static_cast<unsigned long long>(at));
            }
            return;
        }
    }

    std::fprintf(stderr, "STOP: guest raised debug exception code=0x%x argument=0x%llx\n",
                 code, static_cast<unsigned long long>(argument));
    constexpr uint64_t text_lo = 0x800000000ULL;
    constexpr uint64_t text_hi = 0x800000000ULL + 0x50d96dcULL;
    const uint64_t caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    std::fprintf(stderr, "STOP: called from %s0x%llx; game code up the stack:",
                 (caller >= text_lo && caller < text_hi) ? "+" : "host ",
                 static_cast<unsigned long long>((caller >= text_lo && caller < text_hi) ? (caller - text_lo) : caller));

    const auto* stack = static_cast<const uint64_t*>(__builtin_frame_address(0));
    for (int i = 0, found = 0; i < 256 && found < 12; ++i) {
        if (stack[i] >= text_lo && stack[i] < text_hi) {
            std::fprintf(stderr, " +0x%llx", static_cast<unsigned long long>(stack[i] - text_lo));
            ++found;
        }
    }
    std::fprintf(stderr, "\n");
    runtime_report();
    std::fflush(nullptr);
    _exit(23);
}

static ABI int32_t print_backtrace(void) {
    std::fputs("Runtime: guest requested a backtrace (not available)\n", stderr);
    return 0;
}

static ABI uintptr_t guest_signal(int sig, uintptr_t handler) {
    if (sig <= 0 || sig >= 128) {
        *runtime_errno() = 22;
        return static_cast<uintptr_t>(-1);
    }
    uintptr_t old = g_signal_handlers[sig];
    g_signal_handlers[sig] = handler;
    std::printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n", sig);
    return old;
}

static ABI int32_t guest_sigprocmask(int how, const GuestSigset *set, GuestSigset *old) {
    if (old) *old = t_signal_mask;
    if (!set) return 0;
    for (int i = 0; i < 4; ++i) {
        if (how == 1) t_signal_mask.bits[i] |= set->bits[i];
        else if (how == 2) t_signal_mask.bits[i] &= ~set->bits[i];
        else if (how == 3) t_signal_mask.bits[i] = set->bits[i];
        else return FailPosix(EINVAL);
    }
    return 0;
}

static ABI int32_t guest_sigfillset(GuestSigset *set) {
    if (!set) return FailPosix(EINVAL);
    std::memset(set, 0xff, sizeof(*set));
    return 0;
}

static ABI int32_t guest_sigemptyset(GuestSigset *set) {
    if (!set) return FailPosix(EINVAL);
    std::memset(set, 0, sizeof(*set));
    return 0;
}

static ABI int32_t guest_getrusage(int who, GuestRusage *out) {
    struct rusage r{};
    if (!out || (who != 0 && who != 1)) return FailPosix(EINVAL);
    getrusage(who == 0 ? RUSAGE_SELF : RUSAGE_THREAD, &r);
    std::memset(out, 0, sizeof(*out));
    out->utime = GuestTimeval{r.ru_utime.tv_sec, r.ru_utime.tv_usec};
    out->stime = GuestTimeval{r.ru_stime.tv_sec, r.ru_stime.tv_usec};
    return 0;
}

static ABI int32_t guest_sysctl(const int32_t *name, uint32_t namelen, void *old, uint64_t *oldlen,
                                const void *new_value, uint64_t /*newlen*/) {
    if (!name || namelen < 2 || new_value) return FailPosix(EINVAL);
    if (name[0] == 1 && name[1] == 37) { // kern.arandom
        if (!old || !oldlen) return FailPosix(EINVAL);
        if (getrandom(old, static_cast<size_t>(*oldlen), 0) < 0) return FailPosix(errno);
        return 0;
    }
    if (name[0] == 6 && (name[1] == 7 || name[1] == 3)) { // hw.pagesize / hw.ncpu
        if (!oldlen) return FailPosix(EINVAL);
        int32_t value = (name[1] == 7) ? PAGE : 7;
        if (old) {
            if (*oldlen < 4) return FailPosix(ENOMEM);
            std::memcpy(old, &value, 4);
        }
        *oldlen = 4;
        return 0;
    }
    std::fprintf(stderr, "STOP: unsupported sysctl mib");
    for (uint32_t i = 0; i < namelen && i < 8; ++i) {
        std::fprintf(stderr, " %d", name[i]);
    }
    std::fputc('\n', stderr);
    std::exit(21);
}

static ABI int32_t thread_once(int32_t *once, void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    auto* atom = reinterpret_cast<std::atomic<int32_t>*>(once);
    for (;;) {
        int32_t state = atom->load(std::memory_order_acquire);
        if (state == 1) return 0;
        if (state == 0 && atom->compare_exchange_weak(state, 2, std::memory_order_acq_rel, std::memory_order_acquire)) {
            routine();
            atom->store(1, std::memory_order_release);
            return 0;
        }
        sched_yield();
    }
}

static ABI int32_t posix_once(int32_t *once, void (ABI *routine)(void)) {
    return thread_once(once, routine) ? 22 : 0;
}

static ABI int32_t key_create(uint32_t *key, KeyDestructor destructor) {
    return KernelService::Instance().KeyCreate(key, destructor);
}

static ABI int32_t key_delete(uint32_t key) {
    return KernelService::Instance().KeyDelete(key);
}

static ABI int32_t key_set(uint32_t key, void *value) {
    if (!key || key >= KEYS_COUNT || !KernelService::Instance().IsKeyUsed(key)) return ERR(22);
    t_key_values[key] = value;
    return 0;
}

static ABI void *key_get(uint32_t key) {
    return (key && key < KEYS_COUNT) ? t_key_values[key] : nullptr;
}

void runtime_thread_keys_cleanup(void) {
    for (int round = 0; round < 4; ++round) {
        bool any = false;
        for (uint32_t i = 1; i < KEYS_COUNT; ++i) {
            void* value = t_key_values[i];
            auto dtor = KernelService::Instance().GetKeyDestructor(i);
            if (!value || !dtor) continue;
            t_key_values[i] = nullptr;
            any = true;
            dtor(value);
        }
        if (!any) break;
    }
}

static ABI int32_t posix_key_create(uint32_t *k, KeyDestructor d) {
    int32_t r = key_create(k, d);
    return r ? (r & 0xffff) : 0;
}

static ABI int32_t posix_key_delete(uint32_t k) {
    int32_t r = key_delete(k);
    return r ? (r & 0xffff) : 0;
}

static ABI int32_t posix_key_set(uint32_t k, void *v) {
    int32_t r = key_set(k, v);
    return r ? (r & 0xffff) : 0;
}

void runtime_wait_note(int kind, uint64_t ns) {
    static int enabled = -1;
    if (enabled < 0) enabled = (std::getenv("BB_FRAME_STATS") != nullptr);
    if (!enabled) return;
    if (!t_wait_tid) t_wait_tid = static_cast<int>(syscall(SYS_gettid));

    uint64_t sites[3];
    guest_call_sites(sites);
    const uint64_t key = ((sites[0] << 20) ^ (sites[1] * 0x9E3779B1ULL) ^ (sites[2] << 7) ^
                          (static_cast<uint64_t>(t_wait_tid) << 4) ^ static_cast<uint64_t>(kind)) | 1ULL;

    for (uint64_t i = 0, slot = (key * 0x9E3779B97F4A7C15ULL) >> 55; i < 512; ++i) {
        WaitSite* w = &g_wait_sites[(slot + i) % 512];
        uint64_t expected = 0;
        if (w->key.load(std::memory_order_relaxed) == key ||
            w->key.compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
            if (!w->tid) {
                w->site = sites[0];
                w->site2 = sites[1];
                w->site3 = sites[2];
                w->kind = kind;
                pthread_getname_np(pthread_self(), w->name, sizeof(w->name));
                w->tid = t_wait_tid;
            }
            w->count.fetch_add(1, std::memory_order_relaxed);
            w->ns.fetch_add(ns, std::memory_order_relaxed);
            return;
        }
    }
}

void runtime_wait_report(double frames) {
    sample_start();
    sample_report();

    static constexpr std::array<const char*, 4> kinds = {"cond", "mutex", "sema", "sleep"};
    struct Row {
        WaitSite* w{nullptr};
        uint64_t ns{0};
        uint64_t count{0};
    };
    std::array<Row, 512> rows{};
    int n = 0;

    for (size_t i = 0; i < 512; ++i) {
        uint64_t ns = g_wait_sites[i].ns.exchange(0, std::memory_order_relaxed);
        uint64_t count = g_wait_sites[i].count.exchange(0, std::memory_order_relaxed);
        if (count) {
            rows[n++] = Row{&g_wait_sites[i], ns, count};
        }
    }
    if (!n || frames <= 0.0) return;

    static FILE* dump = nullptr;
    static bool dump_checked = false;
    if (!dump_checked) {
        if (const char* path = std::getenv("BB_WAIT_LOG"); path && *path) {
            dump = std::fopen(path, "w");
        }
        dump_checked = true;
    }
    if (dump) {
        for (int i = 0; i < n; ++i) {
            std::fprintf(dump, "%s %d %s %#lx %#lx %#lx %.2f %.3f\n",
                         rows[i].w->name, rows[i].w->tid, kinds[rows[i].w->kind & 3],
                         static_cast<unsigned long>(rows[i].w->site),
                         static_cast<unsigned long>(rows[i].w->site2),
                         static_cast<unsigned long>(rows[i].w->site3),
                         rows[i].count / frames, rows[i].ns / (frames * 1e6));
        }
        std::fprintf(dump, "--\n");
        std::fflush(dump);
    }

    for (int i = 1; i < n; ++i) {
        for (int j = i; j > 0 && rows[j].ns > rows[j - 1].ns; --j) {
            std::swap(rows[j], rows[j - 1]);
        }
    }

    std::printf("Guest waits per frame (thread kind +site):");
    for (int i = 0; i < n && i < 12; ++i) {
        if (rows[i].ns / rows[i].count < 5000000) {
            std::printf("%s %s %s +%#lx<+%#lx<+%#lx %.1fx %.2f ms",
                        i ? ";" : "", rows[i].w->name, kinds[rows[i].w->kind & 3],
                        static_cast<unsigned long>(rows[i].w->site),
                        static_cast<unsigned long>(rows[i].w->site2),
                        static_cast<unsigned long>(rows[i].w->site3),
                        rows[i].count / frames, rows[i].ns / (frames * 1e6));
        }
    }
    std::printf("\n");
}

void runtime_guest_call_sites(uint64_t out[3]) {
    guest_call_sites(out);
}

static const RuntimeExport kernel_exports[] = {
    {"sceKernelClockGettime", reinterpret_cast<void*>(kernel_clock_gettime)},
    {"clock_gettime", reinterpret_cast<void*>(posix_clock_gettime)},
    {"clock_getres", reinterpret_cast<void*>(posix_clock_getres)},
    {"sceKernelGetProcessTime", reinterpret_cast<void*>(process_time)},
    {"sceKernelGetProcessTimeCounter", reinterpret_cast<void*>(process_time_counter)},
    {"sceKernelGetProcessTimeCounterFrequency", reinterpret_cast<void*>(process_time_frequency)},
    {"sceKernelReadTsc", reinterpret_cast<void*>(read_tsc)},
    {"sceKernelGetTscFrequency", reinterpret_cast<void*>(tsc_frequency)},
    {"sceKernelUsleep", reinterpret_cast<void*>(kernel_usleep)},
    {"usleep", reinterpret_cast<void*>(posix_usleep)},
    {"sleep", reinterpret_cast<void*>(posix_sleep)},
    {"nanosleep", reinterpret_cast<void*>(posix_nanosleep)},
    {"sceKernelNanosleep", reinterpret_cast<void*>(kernel_nanosleep)},
    {"sceKernelGettimezone", reinterpret_cast<void*>(kernel_gettimezone)},
    {"gettimeofday", reinterpret_cast<void*>(posix_gettimeofday)},
    {"sceKernelGettimeofday", reinterpret_cast<void*>(kernel_gettimeofday)},
    {"time", reinterpret_cast<void*>(posix_time)},
    {"getpagesize", reinterpret_cast<void*>(get_pagesize)},
    {"getpid", reinterpret_cast<void*>(get_pid)},
    {"sched_yield", reinterpret_cast<void*>(yield)},
    {"_exit", reinterpret_cast<void*>(hard_exit)},
    {"sceKernelDebugRaiseException", reinterpret_cast<void*>(raise_exception)},
    {"sceKernelDebugRaiseExceptionOnReleaseMode", reinterpret_cast<void*>(raise_exception)},
    {"sceKernelPrintBacktraceWithModuleInfo", reinterpret_cast<void*>(print_backtrace)},
    {"signal", reinterpret_cast<void*>(guest_signal)},
    {"sigprocmask", reinterpret_cast<void*>(guest_sigprocmask)},
    {"_sigprocmask", reinterpret_cast<void*>(guest_sigprocmask)},
    {"sigfillset", reinterpret_cast<void*>(guest_sigfillset)},
    {"sigemptyset", reinterpret_cast<void*>(guest_sigemptyset)},
    {"getrusage", reinterpret_cast<void*>(guest_getrusage)},
    {"sysctl", reinterpret_cast<void*>(guest_sysctl)},
    {"scePthreadOnce", reinterpret_cast<void*>(thread_once)},
    {"pthread_once", reinterpret_cast<void*>(posix_once)},
    {"scePthreadKeyCreate", reinterpret_cast<void*>(key_create)},
    {"scePthreadKeyDelete", reinterpret_cast<void*>(key_delete)},
    {"scePthreadSetspecific", reinterpret_cast<void*>(key_set)},
    {"scePthreadGetspecific", reinterpret_cast<void*>(key_get)},
    {"pthread_key_create", reinterpret_cast<void*>(posix_key_create)},
    {"pthread_key_delete", reinterpret_cast<void*>(posix_key_delete)},
    {"pthread_setspecific", reinterpret_cast<void*>(posix_key_set)},
    {"pthread_getspecific", reinterpret_cast<void*>(key_get)},
};

void runtime_kernel_register(void) {
    runtime_register_exports(kernel_exports, sizeof(kernel_exports) / sizeof(kernel_exports[0]));
}

uintptr_t runtime_kernel_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

} // extern "C"

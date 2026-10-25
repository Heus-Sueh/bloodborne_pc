#include "runtime_mutex.hpp"
#include "runtime.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>

namespace bb::runtime {

struct GuestAttr {
    int type{1};
};

struct GuestMutex {
    pthread_mutex_t native;
};

struct GuestCond {
    pthread_cond_t native;
};

struct GuestTimespec {
    int64_t sec{0};
    int64_t nsec{0};
};

class MutexManager {
public:
    static MutexManager& Instance() {
        static MutexManager instance;
        return instance;
    }

    int32_t AttrInit(GuestAttr** out) {
        if (!out) return OrbisError(EINVAL);
        auto* attr = new (std::nothrow) GuestAttr();
        if (!attr) return OrbisError(ENOMEM);
        attr->type = 1;
        *out = attr;
        return 0;
    }

    int32_t AttrType(GuestAttr** attr, int type) {
        if (!attr || !*attr || type < 1 || type > 4) return OrbisError(EINVAL);
        (*attr)->type = type;
        return 0;
    }

    int32_t AttrProtocol(GuestAttr** attr, int protocol) {
        if (!attr || !*attr || protocol < 0 || protocol > 2) return OrbisError(EINVAL);
        return 0;
    }

    int32_t AttrDestroy(GuestAttr** attr) {
        if (!attr || !*attr) return OrbisError(EINVAL);
        delete *attr;
        *attr = nullptr;
        return 0;
    }

    int32_t MutexInit(GuestMutex** out, GuestAttr** attr, const char* /*name*/) {
        if (!out || (attr && !*attr)) return OrbisError(EINVAL);
        int type = attr ? (*attr)->type : 1;
        if (type < 1 || type > 4) return OrbisError(EINVAL);

        auto* mutex = new (std::nothrow) GuestMutex();
        if (!mutex) return OrbisError(ENOMEM);

        pthread_mutexattr_t native_attr;
        int e = pthread_mutexattr_init(&native_attr);
        if (e) {
            delete mutex;
            return OrbisError(e);
        }

        int native_type = (type == 2) ? PTHREAD_MUTEX_RECURSIVE :
                          (type == 3) ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
        e = pthread_mutexattr_settype(&native_attr, native_type);
        if (!e) {
            e = pthread_mutex_init(&mutex->native, &native_attr);
        }
        pthread_mutexattr_destroy(&native_attr);

        if (e) {
            delete mutex;
            return OrbisError(e);
        }

        *out = mutex;
        created_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    int32_t EnsureMutex(GuestMutex** mutex) {
        if (!mutex) return OrbisError(EINVAL);
        auto* ptr_val = reinterpret_cast<std::atomic<uintptr_t>*>(mutex);
        uintptr_t value = ptr_val->load(std::memory_order_acquire);
        if (value == 2) return OrbisError(EINVAL);
        if (value >= 2) return 0;

        std::lock_guard<std::mutex> lock(static_init_mutex_);
        int32_t e = 0;
        if (reinterpret_cast<uintptr_t>(*mutex) < 2) {
            GuestMutex* created_mutex = nullptr;
            e = MutexInit(&created_mutex, nullptr, nullptr);
            if (!e) {
                ptr_val->store(reinterpret_cast<uintptr_t>(created_mutex), std::memory_order_release);
            }
        }
        return e;
    }

    int32_t MutexLock(GuestMutex** mutex) {
        int32_t e = EnsureMutex(mutex);
        if (e) return e;

        int r = pthread_mutex_trylock(&(*mutex)->native);
        if (r == EBUSY) {
            const uint64_t start = runtime_wait_clock();
            r = pthread_mutex_lock(&(*mutex)->native);
            runtime_wait_note(1, runtime_wait_clock() - start);
        }
        e = OrbisError(r);
        if (!e) locks_.fetch_add(1, std::memory_order_relaxed);
        return e;
    }

    int32_t MutexTryLock(GuestMutex** mutex) {
        int32_t e = EnsureMutex(mutex);
        if (e) return e;

        e = OrbisError(pthread_mutex_trylock(&(*mutex)->native));
        if (!e) locks_.fetch_add(1, std::memory_order_relaxed);
        return e;
    }

    int32_t MutexUnlock(GuestMutex** mutex) {
        if (!mutex || reinterpret_cast<uintptr_t>(*mutex) == 2) return OrbisError(EINVAL);
        if (reinterpret_cast<uintptr_t>(*mutex) < 2) return OrbisError(EPERM);

        int32_t e = OrbisError(pthread_mutex_unlock(&(*mutex)->native));
        if (!e) unlocks_.fetch_add(1, std::memory_order_relaxed);
        return e;
    }

    int32_t MutexDestroy(GuestMutex** mutex) {
        if (!mutex || reinterpret_cast<uintptr_t>(*mutex) == 2) return OrbisError(EINVAL);
        if (reinterpret_cast<uintptr_t>(*mutex) < 2) return 0;

        int e = pthread_mutex_destroy(&(*mutex)->native);
        if (!e) {
            delete *mutex;
            *mutex = reinterpret_cast<GuestMutex*>(uintptr_t{2});
        }
        return OrbisError(e);
    }

    int32_t MutexTimedLock(GuestMutex** mutex, uint32_t usec) {
        int32_t e = EnsureMutex(mutex);
        if (e) return e;

        struct timespec end;
        if (DeadlineAfter(&end, usec)) return OrbisError(EINVAL);

        e = TimedError(pthread_mutex_timedlock(&(*mutex)->native, &end));
        if (!e) locks_.fetch_add(1, std::memory_order_relaxed);
        return e;
    }

    int32_t CondInit(GuestCond** out, void** /*attr*/, const char* /*name*/) {
        if (!out) return OrbisError(EINVAL);
        auto* c = new (std::nothrow) GuestCond();
        if (!c) return OrbisError(ENOMEM);

        int e = pthread_cond_init(&c->native, nullptr);
        if (e) {
            delete c;
            return OrbisError(e);
        }

        *out = c;
        conds_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    int32_t EnsureCond(GuestCond** cond) {
        if (!cond) return OrbisError(EINVAL);
        auto* ptr_val = reinterpret_cast<std::atomic<uintptr_t>*>(cond);
        if (ptr_val->load(std::memory_order_acquire) >= 2) return 0;

        std::lock_guard<std::mutex> lock(static_init_mutex_);
        int32_t e = 0;
        if (reinterpret_cast<uintptr_t>(*cond) < 2) {
            GuestCond* c = nullptr;
            e = CondInit(&c, nullptr, nullptr);
            if (!e) {
                ptr_val->store(reinterpret_cast<uintptr_t>(c), std::memory_order_release);
            }
        }
        return e;
    }

    int32_t CondDestroy(GuestCond** cond) {
        if (!cond) return OrbisError(EINVAL);
        if (reinterpret_cast<uintptr_t>(*cond) < 2) return 0;

        int e = pthread_cond_destroy(&(*cond)->native);
        if (!e) {
            delete *cond;
            *cond = nullptr;
        }
        return OrbisError(e);
    }

    int32_t CondWait(GuestCond** cond, GuestMutex** mutex) {
        int32_t e = EnsureCond(cond);
        if (e) return e;
        if (!mutex || reinterpret_cast<uintptr_t>(*mutex) < 3) return OrbisError(EINVAL);

        waits_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t start = runtime_wait_clock();
        const int e2 = pthread_cond_wait(&(*cond)->native, &(*mutex)->native);
        runtime_wait_note(0, runtime_wait_clock() - start);
        return OrbisError(e2);
    }

    int32_t CondWaitUntil(GuestCond** cond, GuestMutex** mutex, const struct timespec* end) {
        int32_t e = EnsureCond(cond);
        if (e) return e;
        if (!mutex || reinterpret_cast<uintptr_t>(*mutex) < 3 || !end) return OrbisError(EINVAL);

        waits_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t start = runtime_wait_clock();
        const int e2 = pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end);
        runtime_wait_note(0, runtime_wait_clock() - start);
        return TimedError(e2);
    }

    int32_t CondTimedWait(GuestCond** cond, GuestMutex** mutex, uint32_t usec) {
        struct timespec end;
        if (DeadlineAfter(&end, usec)) return OrbisError(EINVAL);
        return CondWaitUntil(cond, mutex, &end);
    }

    int32_t CondSignal(GuestCond** cond) {
        int32_t e = EnsureCond(cond);
        if (e) return e;
        wakeups_.fetch_add(1, std::memory_order_relaxed);
        return OrbisError(pthread_cond_signal(&(*cond)->native));
    }

    int32_t CondBroadcast(GuestCond** cond) {
        int32_t e = EnsureCond(cond);
        if (e) return e;
        wakeups_.fetch_add(1, std::memory_order_relaxed);
        return OrbisError(pthread_cond_broadcast(&(*cond)->native));
    }

    int32_t CondAbsTimedWait(GuestCond** cond, GuestMutex** mutex, const GuestTimespec* abs) {
        if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return OrbisError(EINVAL);
        struct timespec end = {
            .tv_sec = static_cast<time_t>(abs->sec),
            .tv_nsec = static_cast<long>(abs->nsec)
        };
        return CondWaitUntil(cond, mutex, &end);
    }

    void Report() const {
        std::printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n",
                    created_.load(std::memory_order_relaxed),
                    locks_.load(std::memory_order_relaxed),
                    unlocks_.load(std::memory_order_relaxed));
        std::printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n",
                    conds_.load(std::memory_order_relaxed),
                    waits_.load(std::memory_order_relaxed),
                    wakeups_.load(std::memory_order_relaxed));
    }

private:
    static int32_t OrbisError(int e) { return orbis_error_from_errno(e); }
    static int32_t TimedError(int e) { return orbis_error_from_timed(e); }

    static int DeadlineAfter(struct timespec* end, uint64_t usec) {
        if (clock_gettime(CLOCK_REALTIME, end)) return errno;
        uint64_t ns = static_cast<uint64_t>(end->tv_nsec) + (usec % 1000000) * 1000;
        end->tv_sec += static_cast<time_t>(usec / 1000000 + ns / 1000000000);
        end->tv_nsec = static_cast<long>(ns % 1000000000);
        return 0;
    }

    std::mutex static_init_mutex_;
    std::atomic<std::size_t> created_{0};
    std::atomic<std::size_t> locks_{0};
    std::atomic<std::size_t> unlocks_{0};
    std::atomic<std::size_t> conds_{0};
    std::atomic<std::size_t> waits_{0};
    std::atomic<std::size_t> wakeups_{0};
};

} // namespace bb::runtime

// Guest ABI Trampolines
extern "C" {

using namespace bb::runtime;

static inline int32_t posix_result(int32_t result) {
    return result ? static_cast<int32_t>(static_cast<uint32_t>(result) & 0xffff) : 0;
}

static ABI int32_t attr_init(GuestAttr **out) { return MutexManager::Instance().AttrInit(out); }
static ABI int32_t attr_type(GuestAttr **attr, int type) { return MutexManager::Instance().AttrType(attr, type); }
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) { return MutexManager::Instance().AttrProtocol(attr, protocol); }
static ABI int32_t attr_destroy(GuestAttr **attr) { return MutexManager::Instance().AttrDestroy(attr); }

static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    return MutexManager::Instance().MutexInit(out, attr, name);
}
static ABI int32_t mutex_lock(GuestMutex **mutex) { return MutexManager::Instance().MutexLock(mutex); }
static ABI int32_t mutex_trylock(GuestMutex **mutex) { return MutexManager::Instance().MutexTryLock(mutex); }
static ABI int32_t mutex_unlock(GuestMutex **mutex) { return MutexManager::Instance().MutexUnlock(mutex); }
static ABI int32_t mutex_destroy(GuestMutex **mutex) { return MutexManager::Instance().MutexDestroy(mutex); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    return MutexManager::Instance().MutexTimedLock(mutex, usec);
}

static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    return MutexManager::Instance().CondInit(out, attr, name);
}
static ABI int32_t cond_destroy(GuestCond **cond) { return MutexManager::Instance().CondDestroy(cond); }
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    return MutexManager::Instance().CondWait(cond, mutex);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    return MutexManager::Instance().CondTimedWait(cond, mutex, usec);
}
static ABI int32_t cond_signal(GuestCond **cond) { return MutexManager::Instance().CondSignal(cond); }
static ABI int32_t cond_broadcast(GuestCond **cond) { return MutexManager::Instance().CondBroadcast(cond); }
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    return MutexManager::Instance().CondAbsTimedWait(cond, mutex, abs);
}

static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr, int type) { return posix_result(attr_type(attr, type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out, GuestAttr **attr) { return posix_result(mutex_init(out, attr, nullptr)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, nullptr)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) {
    return posix_result(cond_abs_timedwait(c, m, t));
}
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }

static const RuntimeExport mutex_exports[] = {
    {"0TyVk4MSLt0", reinterpret_cast<void*>(posix_cond_init)},
    {"RXXqi4CtF8w", reinterpret_cast<void*>(posix_cond_destroy)},
    {"Op8TBGY5KHg", reinterpret_cast<void*>(posix_cond_wait)},
    {"27bAgiJmOh0", reinterpret_cast<void*>(posix_cond_timedwait)},
    {"2MOy+rUfuhQ", reinterpret_cast<void*>(posix_cond_signal)},
    {"mkx2fVhNMsg", reinterpret_cast<void*>(posix_cond_broadcast)},
    {"2Tb92quprl0", reinterpret_cast<void*>(cond_init)},
    {"g+PZd2hiacg", reinterpret_cast<void*>(cond_destroy)},
    {"WKAXJ4XBPQ4", reinterpret_cast<void*>(cond_wait)},
    {"BmMjYxmew1w", reinterpret_cast<void*>(cond_timedwait)},
    {"kDh-NfxgMtE", reinterpret_cast<void*>(cond_signal)},
    {"JGgj7Uvrl+A", reinterpret_cast<void*>(cond_broadcast)},
    {"IafI2PxcPnQ", reinterpret_cast<void*>(mutex_timedlock)},
    {"dQHWEsJtoE4", reinterpret_cast<void*>(posix_attr_init)},
    {"mDmgMOGVUqg", reinterpret_cast<void*>(posix_attr_type)},
    {"HF7lK46xzjY", reinterpret_cast<void*>(posix_attr_destroy)},
    {"ttHNfU+qDBU", reinterpret_cast<void*>(posix_mutex_init)},
    {"ltCfaGr2JGE", reinterpret_cast<void*>(posix_mutex_destroy)},
    {"7H0iTOciTLo", reinterpret_cast<void*>(posix_mutex_lock)},
    {"K-jXhbt2gn4", reinterpret_cast<void*>(posix_mutex_trylock)},
    {"2Z+PpY6CaJg", reinterpret_cast<void*>(posix_mutex_unlock)},
    {"F8bUHwAG284", reinterpret_cast<void*>(attr_init)},
    {"iMp8QpE+XO4", reinterpret_cast<void*>(attr_type)},
    {"smWEktiyyG0", reinterpret_cast<void*>(attr_destroy)},
    {"1FGvU0i9saQ", reinterpret_cast<void*>(attr_protocol)},
    {"cmo1RIYva9o", reinterpret_cast<void*>(mutex_init)},
    {"9UK1vLZQft4", reinterpret_cast<void*>(mutex_lock)},
    {"upoVrzMHFeE", reinterpret_cast<void*>(mutex_trylock)},
    {"tn3VlD0hG60", reinterpret_cast<void*>(mutex_unlock)},
    {"2Of0f+3mhhE", reinterpret_cast<void*>(mutex_destroy)},
};

void runtime_mutex_register(void) {
    runtime_register_exports(mutex_exports, sizeof(mutex_exports) / sizeof(mutex_exports[0]));
}

uintptr_t runtime_mutex_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_mutex_report(void) {
    MutexManager::Instance().Report();
}

} // extern "C"

#include "runtime_rwlock.hpp"
#include "runtime.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <list>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

namespace bb::runtime {

struct GuestTime {
    int64_t seconds{0};
    int64_t nanoseconds{0};
};

struct Holder {
    pthread_t thread{0};
    unsigned readers{0};
    unsigned writer{0};
    unsigned pending{0};
};

struct Rwlock {
    pthread_rwlock_t native;
    std::list<Holder> holders;
    unsigned inflight{0};

    Rwlock() {
        pthread_rwlock_init(&native, nullptr);
    }
    ~Rwlock() {
        pthread_rwlock_destroy(&native);
    }
};

enum class Operation {
    Read,
    Write,
    TryRead,
    TryWrite,
    TimedRead,
    TimedWrite
};

class RwlockManager {
public:
    static RwlockManager& Instance() {
        static RwlockManager instance;
        return instance;
    }

    int32_t Init(Rwlock** out, void* const* attr, const char* /*name*/) {
        if (!out || (attr && !*attr)) return Error(EINVAL);
        if (attr) {
            std::fputs("STOP: non-default PS4 rwlock attributes are not implemented\n", stderr);
            std::exit(21);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto r = std::make_shared<Rwlock>();
        registry_[reinterpret_cast<uintptr_t>(r.get())] = r;
        *out = r.get();
        ++created_;
        return 0;
    }

    int32_t Acquire(Rwlock** handle, Operation op, const GuestTime* time) {
        if (!handle) return Error(EINVAL);

        const bool writer = (op == Operation::Write || op == Operation::TryWrite || op == Operation::TimedWrite);
        const bool attempt = (op == Operation::TryRead || op == Operation::TryWrite);
        const bool timed = (op == Operation::TimedRead || op == Operation::TimedWrite);

        std::shared_ptr<Rwlock> r;
        const pthread_t self = pthread_self();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!*handle) {
                auto new_r = std::make_shared<Rwlock>();
                registry_[reinterpret_cast<uintptr_t>(new_r.get())] = new_r;
                *handle = new_r.get();
                ++created_;
            }

            auto it = registry_.find(reinterpret_cast<uintptr_t>(*handle));
            if (it == registry_.end()) {
                return Error(EINVAL);
            }
            r = it->second;

            Holder* h = FindHolderLocked(*r, self);
            if (h && (h->writer || (writer && h->readers))) {
                return Error(attempt ? EBUSY : EDEADLK);
            }
            if (h && h->readers == UINT_MAX) {
                return Error(EAGAIN);
            }

            if (!h) {
                r->holders.push_back(Holder{.thread = self});
                h = &r->holders.back();
            }

            ++r->inflight;
            ++h->pending;
        }

        int e = 0;
        if (attempt || timed) {
            e = writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
        } else {
            e = writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
        }

        if (timed && e == EBUSY) {
            if (!time || time->nanoseconds < 0 || time->nanoseconds >= 1000000000) {
                e = EINVAL;
            } else {
                struct timespec deadline = {
                    .tv_sec = static_cast<time_t>(time->seconds),
                    .tv_nsec = static_cast<long>(time->nanoseconds)
                };
                e = writer ? pthread_rwlock_timedwrlock(&r->native, &deadline)
                           : pthread_rwlock_timedrdlock(&r->native, &deadline);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --r->inflight;
            Holder* h = FindHolderLocked(*r, self);
            if (h) {
                --h->pending;
                if (!e) {
                    if (writer) {
                        h->writer = 1;
                        ++writes_;
                    } else {
                        ++h->readers;
                        ++reads_;
                    }
                }
                PruneHolderLocked(*r, h);
            }
        }

        return Error(e);
    }

    int32_t Unlock(Rwlock** handle) {
        if (!handle) return Error(EINVAL);

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(reinterpret_cast<uintptr_t>(*handle));
        if (it == registry_.end()) return Error(EINVAL);

        auto r = it->second;
        const pthread_t self = pthread_self();
        Holder* h = FindHolderLocked(*r, self);

        if (!h || (!h->readers && !h->writer)) {
            return Error(EPERM);
        }

        int e = pthread_rwlock_unlock(&r->native);
        if (!e) {
            if (h->writer) {
                h->writer = 0;
            } else {
                --h->readers;
            }
            ++unlocks_;
        }

        PruneHolderLocked(*r, h);
        return Error(e);
    }

    int32_t Destroy(Rwlock** handle) {
        if (!handle) return Error(EINVAL);

        std::lock_guard<std::mutex> lock(mutex_);
        if (!*handle) return 0;

        auto it = registry_.find(reinterpret_cast<uintptr_t>(*handle));
        if (it == registry_.end()) return Error(EINVAL);

        auto r = it->second;
        if (r->inflight || !r->holders.empty()) {
            return Error(EBUSY);
        }

        registry_.erase(it);
        *handle = reinterpret_cast<Rwlock*>(uintptr_t{1});
        return 0;
    }

    void Report() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",
                    created_, reads_, writes_, unlocks_);
    }

private:
    static int32_t Error(int e) { return orbis_error_from_errno(e); }

    static Holder* FindHolderLocked(Rwlock& r, pthread_t self) {
        for (auto& holder : r.holders) {
            if (pthread_equal(holder.thread, self)) {
                return &holder;
            }
        }
        return nullptr;
    }

    static void PruneHolderLocked(Rwlock& r, Holder* h) {
        if (h && h->readers == 0 && h->writer == 0 && h->pending == 0) {
            for (auto it = r.holders.begin(); it != r.holders.end(); ++it) {
                if (&(*it) == h) {
                    r.holders.erase(it);
                    break;
                }
            }
        }
    }

    std::mutex mutex_;
    std::unordered_map<uintptr_t, std::shared_ptr<Rwlock>> registry_;

    std::size_t created_{0};
    std::size_t reads_{0};
    std::size_t writes_{0};
    std::size_t unlocks_{0};
};

} // namespace bb::runtime

// Guest ABI Trampolines
extern "C" {

using namespace bb::runtime;

static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    return RwlockManager::Instance().Init(out, attr, name);
}

static ABI int32_t rw_read(Rwlock **r) {
    return RwlockManager::Instance().Acquire(r, Operation::Read, nullptr);
}

static ABI int32_t rw_write(Rwlock **r) {
    return RwlockManager::Instance().Acquire(r, Operation::Write, nullptr);
}

static ABI int32_t rw_tryread(Rwlock **r) {
    return RwlockManager::Instance().Acquire(r, Operation::TryRead, nullptr);
}

static ABI int32_t rw_trywrite(Rwlock **r) {
    return RwlockManager::Instance().Acquire(r, Operation::TryWrite, nullptr);
}

static ABI int32_t rw_timedread(Rwlock **r, const GuestTime *t) {
    return RwlockManager::Instance().Acquire(r, Operation::TimedRead, t);
}

static ABI int32_t rw_timedwrite(Rwlock **r, const GuestTime *t) {
    return RwlockManager::Instance().Acquire(r, Operation::TimedWrite, t);
}

static ABI int32_t rw_unlock(Rwlock **handle) {
    return RwlockManager::Instance().Unlock(handle);
}

static ABI int32_t rw_destroy(Rwlock **handle) {
    return RwlockManager::Instance().Destroy(handle);
}

static const RuntimeExport rwlock_exports[] = {
    {"6ULAa0fq4jA", reinterpret_cast<void*>(rw_init)},
    {"BB+kb08Tl9A", reinterpret_cast<void*>(rw_destroy)},
    {"Ox9i0c7L5w0", reinterpret_cast<void*>(rw_read)},
    {"mqdNorrB+gI", reinterpret_cast<void*>(rw_write)},
    {"XD3mDeybCnk", reinterpret_cast<void*>(rw_tryread)},
    {"bIHoZCTomsI", reinterpret_cast<void*>(rw_trywrite)},
    {"+L98PIbGttk", reinterpret_cast<void*>(rw_unlock)},
    {"iPtZRWICjrM", reinterpret_cast<void*>(rw_timedread)},
    {"adh--6nIqTk", reinterpret_cast<void*>(rw_timedwrite)},
};

void runtime_rwlock_register(void) {
    runtime_register_exports(rwlock_exports, sizeof(rwlock_exports) / sizeof(rwlock_exports[0]));
}

uintptr_t runtime_rwlock_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_rwlock_report(void) {
    RwlockManager::Instance().Report();
}

} // extern "C"

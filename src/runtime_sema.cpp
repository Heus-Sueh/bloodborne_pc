#include "runtime_sema.hpp"
#include "runtime.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define ERR(n) ORBIS_ERR(n)

namespace bb::runtime {

struct Waiter {
    int32_t need{0};
    int32_t result{0};
    bool done{false};
    std::condition_variable cond;
};

struct Semaphore {
    uint32_t id{0};
    uint32_t attr{0};
    int32_t count{0};
    int32_t initial{0};
    int32_t maximum{0};
    bool deleted{false};
    unsigned active{0};
    std::vector<std::shared_ptr<Waiter>> waiters;
};

class SemaphoreManager {
public:
    static SemaphoreManager& Instance() {
        static SemaphoreManager instance;
        return instance;
    }

    int32_t Create(uint32_t* out, const char* name, uint32_t attr,
                   int32_t initial, int32_t maximum, const void* options) {
        if (!out || !name || attr > 2 || initial < 0 || maximum <= 0 || initial > maximum) {
            return ERR(22);
        }
        if (options) {
            std::fputs("STOP: semaphore options are not implemented\n", stderr);
            std::exit(21);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (next_id_ == UINT32_MAX) {
            return ERR(28);
        }

        auto sem = std::make_shared<Semaphore>();
        sem->id = next_id_++;
        sem->attr = attr;
        sem->count = initial;
        sem->initial = initial;
        sem->maximum = maximum;

        registry_[sem->id] = sem;
        *out = sem->id;
        ++created_;

        std::printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",
                    sem->id, attr, initial, maximum);
        return 0;
    }

    int32_t Wait(uint32_t id, int32_t need, uint32_t* timeout, bool block) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end() || it->second->deleted) {
            return ERR(3);
        }

        auto sem = it->second;
        if (need <= 0 || need > sem->maximum) {
            return ERR(22);
        }

        if (sem->count >= need) {
            sem->count -= need;
            ++acquired_;
            return 0;
        }

        if (!block) {
            return ERR(16);
        }

        if (timeout && *timeout == 0) {
            ++timed_out_;
            return ERR(60);
        }

        if (const char* trace = std::getenv("BB_TRACE_SEMA"); trace && trace[0]) {
            std::fprintf(stderr, "Runtime: blocking wait on semaphore %u (need %d, count %d)\n",
                         id, need, sem->count);
        }

        auto waiter = std::make_shared<Waiter>();
        waiter->need = need;
        sem->waiters.push_back(waiter);
        ++sem->active;

        const uint64_t wait_start = runtime_wait_clock();
        const auto start_time = std::chrono::steady_clock::now();
        const bool has_timeout = (timeout != nullptr);
        const auto timeout_duration = has_timeout ? std::chrono::microseconds(*timeout) : std::chrono::microseconds(0);
        const auto deadline = start_time + timeout_duration;

        while (!waiter->done) {
            if (has_timeout) {
                if (waiter->cond.wait_until(lock, deadline) == std::cv_status::timeout && !waiter->done) {
                    // Timeout occurred: remove from waiter list
                    for (auto w_it = sem->waiters.begin(); w_it != sem->waiters.end(); ++w_it) {
                        if (*w_it == waiter) {
                            sem->waiters.erase(w_it);
                            break;
                        }
                    }
                    waiter->result = ERR(60);
                    waiter->done = true;
                    ++timed_out_;
                }
            } else {
                waiter->cond.wait(lock);
            }
        }

        runtime_wait_note(2, runtime_wait_clock() - wait_start);
        int32_t result = waiter->result;
        if (!result) {
            ++acquired_;
        }

        if (has_timeout) {
            const auto now = std::chrono::steady_clock::now();
            if (result != 0 || now >= deadline) {
                *timeout = 0;
            } else {
                *timeout = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count());
            }
        }

        if (--sem->active == 0 && sem->deleted) {
            registry_.erase(id);
        }

        return result;
    }

    int32_t Signal(uint32_t id, int32_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end() || it->second->deleted) {
            return ERR(3);
        }

        auto sem = it->second;
        if (count <= 0 || count > sem->maximum - sem->count) {
            return ERR(22);
        }

        sem->count += count;
        ++signaled_;

        auto w_it = sem->waiters.begin();
        while (w_it != sem->waiters.end()) {
            auto waiter = *w_it;
            if (waiter->need > sem->count) {
                ++w_it;
                continue;
            }

            sem->count -= waiter->need;
            w_it = sem->waiters.erase(w_it);
            waiter->done = true;
            waiter->cond.notify_one();
        }

        return 0;
    }

    int32_t Cancel(uint32_t id, int32_t count, int32_t* waiters_out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end() || it->second->deleted) {
            return ERR(3);
        }

        auto sem = it->second;
        if (count > sem->maximum) {
            return ERR(22);
        }

        int32_t woken = WakeAllLocked(*sem, ERR(85));
        if (waiters_out) {
            *waiters_out = woken;
        }

        sem->count = (count < 0) ? sem->initial : count;
        return 0;
    }

    int32_t Delete(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end() || it->second->deleted) {
            return ERR(3);
        }

        auto sem = it->second;
        sem->deleted = true;
        ++deleted_;

        WakeAllLocked(*sem, ERR(13));

        if (sem->active == 0) {
            registry_.erase(it);
        }
        return 0;
    }

    unsigned WaitersCount(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end()) return 0;
        return static_cast<unsigned>(it->second->waiters.size());
    }

    void Report() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
                    created_, deleted_, acquired_, signaled_, timed_out_);
    }

private:
    int32_t WakeAllLocked(Semaphore& sem, int32_t result) {
        int32_t count = 0;
        for (auto& waiter : sem.waiters) {
            waiter->result = result;
            waiter->done = true;
            ++count;
            waiter->cond.notify_one();
        }
        sem.waiters.clear();
        return count;
    }

    std::mutex mutex_;
    uint32_t next_id_{1};
    std::unordered_map<uint32_t, std::shared_ptr<Semaphore>> registry_;

    std::size_t created_{0};
    std::size_t deleted_{0};
    std::size_t acquired_{0};
    std::size_t signaled_{0};
    std::size_t timed_out_{0};
};

} // namespace bb::runtime

// Guest ABI Trampolines
extern "C" {

static ABI int32_t sem_create(uint32_t *out, const char *name, uint32_t attr,
                              int32_t initial, int32_t maximum, const void *options) {
    return bb::runtime::SemaphoreManager::Instance().Create(out, name, attr, initial, maximum, options);
}

static ABI int32_t sem_wait(uint32_t id, int32_t need, uint32_t *timeout) {
    return bb::runtime::SemaphoreManager::Instance().Wait(id, need, timeout, true);
}

static ABI int32_t sem_poll(uint32_t id, int32_t need) {
    return bb::runtime::SemaphoreManager::Instance().Wait(id, need, nullptr, false);
}

static ABI int32_t sem_signal(uint32_t id, int32_t count) {
    return bb::runtime::SemaphoreManager::Instance().Signal(id, count);
}

static ABI int32_t sem_cancel(uint32_t id, int32_t count, int32_t *waiters) {
    return bb::runtime::SemaphoreManager::Instance().Cancel(id, count, waiters);
}

static ABI int32_t sem_delete(uint32_t id) {
    return bb::runtime::SemaphoreManager::Instance().Delete(id);
}

unsigned runtime_sema_waiters(uint32_t id) {
    return bb::runtime::SemaphoreManager::Instance().WaitersCount(id);
}

void runtime_sema_report(void) {
    bb::runtime::SemaphoreManager::Instance().Report();
}

uintptr_t runtime_sema_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

static const RuntimeExport sema_exports[] = {
    {"188x57JYp0g", reinterpret_cast<void*>(sem_create)},
    {"Zxa0VhQVTsk", reinterpret_cast<void*>(sem_wait)},
    {"4czppHBiriw", reinterpret_cast<void*>(sem_signal)},
    {"12wOHk8ywb0", reinterpret_cast<void*>(sem_poll)},
    {"4DM06U2BNEY", reinterpret_cast<void*>(sem_cancel)},
    {"R1Jvn8bSCW8", reinterpret_cast<void*>(sem_delete)},
};

void runtime_sema_register(void) {
    runtime_register_exports(sema_exports, sizeof(sema_exports) / sizeof(sema_exports[0]));
}

} // extern "C"

#ifndef BB_PLATFORM_SYNC_H
#define BB_PLATFORM_SYNC_H

#include "platform.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if defined(BB_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef SRWLOCK BbPlatformMutex;
#define BB_PLATFORM_MUTEX_INIT SRWLOCK_INIT
static inline void bb_platform_mutex_init(BbPlatformMutex *m) { InitializeSRWLock(m); }
static inline void bb_platform_mutex_lock(BbPlatformMutex *m) { AcquireSRWLockExclusive(m); }
static inline int bb_platform_mutex_trylock(BbPlatformMutex *m) { return TryAcquireSRWLockExclusive(m) != 0; }
static inline void bb_platform_mutex_unlock(BbPlatformMutex *m) { ReleaseSRWLockExclusive(m); }

typedef struct {
    SRWLOCK lock;
    DWORD owner;
    unsigned depth;
} BbPlatformRecursiveMutex;
#define BB_PLATFORM_RECURSIVE_MUTEX_INIT {SRWLOCK_INIT, 0, 0}
static inline void bb_platform_recursive_mutex_lock(BbPlatformRecursiveMutex *m) {
    DWORD self = GetCurrentThreadId();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) {
        ++m->depth;
        return;
    }
    AcquireSRWLockExclusive(&m->lock);
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
    m->depth = 1;
}
static inline void bb_platform_recursive_mutex_unlock(BbPlatformRecursiveMutex *m) {
    if (--m->depth) return;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&m->lock);
}

typedef SRWLOCK BbPlatformRwlock;
#define BB_PLATFORM_RWLOCK_INIT SRWLOCK_INIT
static inline void bb_platform_rwlock_init(BbPlatformRwlock *l) { InitializeSRWLock(l); }
static inline void bb_platform_rwlock_read_lock(BbPlatformRwlock *l) { AcquireSRWLockShared(l); }
static inline int bb_platform_rwlock_try_read_lock(BbPlatformRwlock *l) { return TryAcquireSRWLockShared(l) != 0; }
static inline void bb_platform_rwlock_read_unlock(BbPlatformRwlock *l) { ReleaseSRWLockShared(l); }
static inline void bb_platform_rwlock_write_lock(BbPlatformRwlock *l) { AcquireSRWLockExclusive(l); }
static inline int bb_platform_rwlock_try_write_lock(BbPlatformRwlock *l) { return TryAcquireSRWLockExclusive(l) != 0; }
static inline void bb_platform_rwlock_write_unlock(BbPlatformRwlock *l) { ReleaseSRWLockExclusive(l); }

typedef CONDITION_VARIABLE BbPlatformCond;
#define BB_PLATFORM_COND_INIT CONDITION_VARIABLE_INIT
static inline void bb_platform_cond_init(BbPlatformCond *c) { InitializeConditionVariable(c); }
static inline void bb_platform_cond_wait(BbPlatformCond *c, BbPlatformMutex *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline int bb_platform_cond_timedwait(BbPlatformCond *c, BbPlatformMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    if (SleepConditionVariableSRW(c, m, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}
static inline void bb_platform_cond_signal(BbPlatformCond *c) { WakeConditionVariable(c); }
static inline void bb_platform_cond_broadcast(BbPlatformCond *c) { WakeAllConditionVariable(c); }

typedef uint64_t BbPlatformThreadId;
static inline BbPlatformThreadId bb_platform_thread_id(void) { return GetCurrentThreadId(); }
static inline int bb_platform_thread_equal(BbPlatformThreadId a, BbPlatformThreadId b) { return a == b; }
static inline void bb_platform_yield(void) { SwitchToThread(); }
typedef HANDLE BbPlatformThread;

#else /* POSIX (Linux, macOS) */
#include <pthread.h>
#include <sched.h>

typedef pthread_mutex_t BbPlatformMutex;
#define BB_PLATFORM_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void bb_platform_mutex_init(BbPlatformMutex *m) { pthread_mutex_init(m, NULL); }
static inline void bb_platform_mutex_lock(BbPlatformMutex *m) { pthread_mutex_lock(m); }
static inline int bb_platform_mutex_trylock(BbPlatformMutex *m) { return pthread_mutex_trylock(m) == 0; }
static inline void bb_platform_mutex_unlock(BbPlatformMutex *m) { pthread_mutex_unlock(m); }

typedef pthread_mutex_t BbPlatformRecursiveMutex;
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
#define BB_PLATFORM_RECURSIVE_MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#else
#define BB_PLATFORM_RECURSIVE_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#endif
static inline void bb_platform_recursive_mutex_lock(BbPlatformRecursiveMutex *m) { pthread_mutex_lock(m); }
static inline void bb_platform_recursive_mutex_unlock(BbPlatformRecursiveMutex *m) { pthread_mutex_unlock(m); }

typedef pthread_rwlock_t BbPlatformRwlock;
#define BB_PLATFORM_RWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void bb_platform_rwlock_init(BbPlatformRwlock *l) { pthread_rwlock_init(l, NULL); }
static inline void bb_platform_rwlock_read_lock(BbPlatformRwlock *l) { pthread_rwlock_rdlock(l); }
static inline int bb_platform_rwlock_try_read_lock(BbPlatformRwlock *l) { return pthread_rwlock_tryrdlock(l) == 0; }
static inline void bb_platform_rwlock_read_unlock(BbPlatformRwlock *l) { pthread_rwlock_unlock(l); }
static inline void bb_platform_rwlock_write_lock(BbPlatformRwlock *l) { pthread_rwlock_wrlock(l); }
static inline int bb_platform_rwlock_try_write_lock(BbPlatformRwlock *l) { return pthread_rwlock_trywrlock(l) == 0; }
static inline void bb_platform_rwlock_write_unlock(BbPlatformRwlock *l) { pthread_rwlock_unlock(l); }

typedef pthread_cond_t BbPlatformCond;
#define BB_PLATFORM_COND_INIT PTHREAD_COND_INITIALIZER
static inline void bb_platform_cond_init(BbPlatformCond *c) { pthread_cond_init(c, NULL); }
static inline void bb_platform_cond_wait(BbPlatformCond *c, BbPlatformMutex *m) { pthread_cond_wait(c, m); }
static inline void bb_platform_cond_signal(BbPlatformCond *c) { pthread_cond_signal(c); }
static inline void bb_platform_cond_broadcast(BbPlatformCond *c) { pthread_cond_broadcast(c); }

typedef pthread_t BbPlatformThreadId;
static inline BbPlatformThreadId bb_platform_thread_id(void) { return pthread_self(); }
static inline int bb_platform_thread_equal(BbPlatformThreadId a, BbPlatformThreadId b) { return pthread_equal(a, b); }
static inline void bb_platform_yield(void) { sched_yield(); }
typedef pthread_t BbPlatformThread;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int bb_platform_thread_start(BbPlatformThread *thread, size_t stack, void *(*entry)(void *), void *argument);
int bb_platform_thread_join(BbPlatformThread thread);
void bb_platform_thread_detach(BbPlatformThread thread);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_SYNC_H */

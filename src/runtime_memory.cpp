/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "src/runtime_memory.hpp"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime::memory {

/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
uint64_t game_pool_bytes() {
    static uint64_t size = 0;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, nullptr, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}

/* bbport: direct memory past the game's size, for the port's own guest allocations
 * (runtime_extra_direct_map: the render resource cache's larger backing). The game neither sees
 * nor allocates it. BB_DMEM_EXTRA_MB (0..2048, default 512). */
uint64_t extra_pool_bytes() {
    static int64_t size = -1;
    if (size < 0) {
        const char *env = getenv("BB_DMEM_EXTRA_MB");
        uint64_t mb = env ? strtoull(env, nullptr, 10) : 512;
        if (mb > 2048) mb = 2048;
        size = static_cast<int64_t>(mb * 1024 * 1024);
    }
    return static_cast<uint64_t>(size);
}

inline uint64_t pool_size_bytes() {
    return game_pool_bytes() + extra_pool_bytes();
}

#define POOL_SIZE pool_size_bytes()
#define GAME_POOL_SIZE game_pool_bytes()

constexpr uint64_t FLEXIBLE_SIZE = UINT64_C(448) * 1024 * 1024;
constexpr uint64_t PAGE = UINT64_C(16384);
constexpr uint64_t USER_MIN = UINT64_C(0x1000000000);
constexpr uint64_t USER_MAX = UINT64_C(0xfc00000000);
constexpr int LIMIT = 4096;

constexpr int32_t INVALID = static_cast<int32_t>(UINT32_C(0x80020016));
constexpr int32_t NO_MEMORY = static_cast<int32_t>(UINT32_C(0x8002000c));
constexpr int32_t ACCESS = static_cast<int32_t>(UINT32_C(0x8002000d));
constexpr int32_t NO_SPACE = static_cast<int32_t>(UINT32_C(0x80020023));

constexpr int MAP_FIXED_FLAG = 0x10;
constexpr int MAP_NO_OVERWRITE = 0x80;

enum { KIND_RESERVED = 1, KIND_DIRECT, KIND_FLEXIBLE };

struct Block {
    uint64_t start{0};
    uint64_t size{0};
    int type{0};
    int used{0};
};

struct Vma {
    uintptr_t start{0};
    uintptr_t end{0};
    int kind{0};
    int prot{0};
    int type{0};
    uint64_t phys{0};
};

struct VirtualQueryInfo {
    uintptr_t start{0};
    uintptr_t end{0};
    uint64_t offset{0};
    int32_t protection{0};
    int32_t memory_type{0};
    uint32_t flags{0};
    char name[32]{};
}; /* flags: flexible, direct, stack, pooled, committed */

struct BatchEntry {
    void *start{nullptr};
    uint64_t offset{0};
    uint64_t length{0};
    int8_t prot{0};
    int8_t type{0};
    int16_t reserved{0};
    int32_t operation{0};
};

static_assert(sizeof(VirtualQueryInfo) == 72, "PS4 virtual query info layout");
static_assert(sizeof(BatchEntry) == 32, "PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
__thread unsigned exclusive_depth = 0;

/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
uint64_t table_generation = 0;

void write_lock() {
    if (!exclusive_depth++) {
        pthread_rwlock_wrlock(&lock);
        __atomic_add_fetch(&table_generation, 1, __ATOMIC_ACQ_REL);
    }
}

void write_unlock() {
    if (!--exclusive_depth) {
        __atomic_add_fetch(&table_generation, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&lock);
    }
}

void read_lock() {
    if (!exclusive_depth) pthread_rwlock_rdlock(&lock);
}

void read_unlock() {
    if (!exclusive_depth) pthread_rwlock_unlock(&lock);
}

Block blocks[LIMIT];
Vma *vmas = nullptr;
size_t vma_count = 0;
size_t vma_capacity = 0;
int pool_fd = -1;

/* bbport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * BB_GUEST_GPU_MEMORY=1): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
constexpr uint64_t CHUNK = (UINT64_C(256) << 20);
using GuestChunkAlloc = int (*)(uint64_t phys, uint64_t size);
GuestChunkAlloc chunk_alloc = nullptr;
int *chunk_fds = nullptr; /* per CHUNK of direct memory: -1 not decided, -2 memfd, -3 memfd the GPU
                           * imported (host memory: VK_EXT_external_memory_host), else a dma-buf fd */
constexpr int CHUNK_IMPORTED = (-3);

#define GPU_CHUNK(fd) ((fd) >= 0 || (fd) == CHUNK_IMPORTED)

unsigned char *backing_base = nullptr; /* second view of the pool: host writes bypass guest/GPU page protection */
int whole_chunks = 0;

struct Region {
    uint64_t phys{0};
    uint64_t size{0};
    int fd{-1};
};

constexpr size_t MAX_REGIONS = 256;
Region regions[MAX_REGIONS];
size_t region_count = 0;

/* The region holding phys (sorted by phys), or nullptr; *next: start of the next region above. */
const Region *region_at(uint64_t phys, uint64_t *next) {
    *next = POOL_SIZE;
    for (size_t i = 0; i < region_count; ++i) {
        if (phys < regions[i].phys) {
            *next = regions[i].phys;
            return nullptr;
        }
        if (phys < regions[i].phys + regions[i].size) return &regions[i];
    }
    return nullptr;
}

void add_region(uint64_t phys, uint64_t size) {
    if (region_count == MAX_REGIONS) return;
    int fd = chunk_alloc ? chunk_alloc(phys, size) : -1;
    if (fd >= 0 && mmap(backing_base + phys, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        close(fd);
        fd = -1;
    }
    size_t i = region_count;
    while (i > 0 && regions[i - 1].phys > phys) {
        regions[i] = regions[i - 1];
        --i;
    }
    regions[i] = Region{phys, size, (fd >= 0 || fd == CHUNK_IMPORTED) ? fd : -2};
    ++region_count;
}

constexpr uint64_t FLEX_SPAN = (UINT64_C(1024) * 1024 * 1024);
uint64_t flex_bitmap[FLEX_SPAN / PAGE / 64] = {0};

/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
GpuRange hook_map = nullptr;
GpuRange hook_unmap = nullptr;
GpuRange hook_invalidate = nullptr;
GpuRange hook_note_write = nullptr; /* bbport: data written into GPU memory (runtime_memory_note_write) */

enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };

struct PendingHook {
    int kind{0};
    uintptr_t address{0};
    uint64_t size{0};
};

PendingHook pending[64];
size_t pending_count = 0;

size_t allocations = 0;
size_t maps = 0;
size_t flexible_maps = 0;
size_t protects = 0;
size_t queries = 0;
uint64_t live_bytes = 0;
uint64_t flexible_bytes = 0;

inline int valid_alignment(uint64_t a) {
    return a >= PAGE && a <= POOL_SIZE && !(a & (a - 1));
}

inline uint64_t align_up(uint64_t n, uint64_t a) {
    return (n + a - 1) & ~(a - 1);
}

inline int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) |
           ((prot & 0x22) ? (PROT_WRITE | PROT_READ) : 0) |
           ((prot & 4) ? (PROT_EXEC | PROT_READ) : 0);
}

/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
int pool() {
    if (pool_fd >= 0) return 0;
#ifdef _WIN32
    /* A pagefile-backed section (gpu/platform/win32/posix_memory.cpp). */
    pool_fd = bb_section_create(POOL_SIZE + FLEX_SPAN);
    if (pool_fd < 0) return -1;
#else
    pool_fd = memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd < 0 || ftruncate(pool_fd, static_cast<off_t>(POOL_SIZE + FLEX_SPAN))) return -1;
#endif
    void *view = mmap(nullptr, POOL_SIZE + FLEX_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, pool_fd, 0);
    if (view == MAP_FAILED) return -1;
    backing_base = static_cast<unsigned char *>(view);
    chunk_fds = static_cast<int *>(malloc((POOL_SIZE / CHUNK + 1) * sizeof(int)));
    if (!chunk_fds) return -1;
    for (uint64_t c = 0; c <= POOL_SIZE / CHUNK; ++c) chunk_fds[c] = -1;
    return 0;
}

/* Size of direct memory chunk c (the last one may be shorter). */
uint64_t chunk_size(uint64_t c) {
    const uint64_t start = c * CHUNK;
    return POOL_SIZE - start < CHUNK ? POOL_SIZE - start : CHUNK;
}

/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
void ensure_chunks(uint64_t phys, uint64_t size) {
    if (!chunk_fds || phys >= POOL_SIZE) return;
    if (whole_chunks) {
        if (!chunk_alloc) return;
        const uint64_t end = (phys + size < POOL_SIZE) ? phys + size : POOL_SIZE;
        for (uint64_t at = phys; at < end;) {
            uint64_t next = 0;
            const Region *r = region_at(at, &next);
            if (r) {
                at = r->phys + r->size;
                continue;
            }
            const uint64_t gap_end = (next < end) ? next : end;
            add_region(at, gap_end - at);
            at = gap_end;
        }
        return;
    }
    const uint64_t last = ((phys + size - 1 < POOL_SIZE) ? phys + size - 1 : POOL_SIZE - 1) / CHUNK;
    for (uint64_t c = phys / CHUNK; c <= last; ++c) {
        if (chunk_fds[c] != -1) continue;
        int fd = chunk_alloc ? chunk_alloc(c * CHUNK, chunk_size(c)) : -1;
        if (fd >= 0 && mmap(backing_base + c * CHUNK, chunk_size(c), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            close(fd);
            fd = -1;
        }
        chunk_fds[c] = (fd >= 0 || fd == CHUNK_IMPORTED) ? fd : -2;
    }
}

/* mmap of direct or flexible memory at phys, split where it crosses chunks. */
void *map_phys(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
    if (!chunk_fds || phys >= POOL_SIZE) {
        return mmap(reinterpret_cast<void *>(address), size, prot, MAP_SHARED | MAP_FIXED, pool_fd, static_cast<off_t>(phys));
    }
    ensure_chunks(phys, size);
    for (uint64_t done = 0; whole_chunks && done < size;) {
        const uint64_t at = phys + done;
        uint64_t next = 0;
        const Region *r = region_at(at, &next);
        const uint64_t room = r ? r->phys + r->size - at : next - at;
        const uint64_t n = (size - done < room) ? size - done : room;
        void *p = (r && r->fd >= 0)
                      ? mmap(reinterpret_cast<void *>(address + done), n, prot, MAP_SHARED | MAP_FIXED, r->fd, static_cast<off_t>(at - r->phys))
                      : mmap(reinterpret_cast<void *>(address + done), n, prot, MAP_SHARED | MAP_FIXED, pool_fd, static_cast<off_t>(at));
        if (p == MAP_FAILED) {
            static int reported = 0;
            if (reported++ < 4) {
                fprintf(stderr, "Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at, n, r && r->fd >= 0 ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd", strerror(errno));
            }
            return MAP_FAILED;
        }
        done += n;
        if (done == size) return reinterpret_cast<void *>(address);
    }
    for (uint64_t done = 0; done < size;) {
        const uint64_t at = phys + done;
        const uint64_t c = at / CHUNK;
        const uint64_t room = c * CHUNK + chunk_size(c) - at;
        const uint64_t n = (size - done < room) ? size - done : room;
        void *p = (chunk_fds[c] >= 0)
                      ? mmap(reinterpret_cast<void *>(address + done), n, prot, MAP_SHARED | MAP_FIXED, chunk_fds[c], static_cast<off_t>(at - c * CHUNK))
                      : mmap(reinterpret_cast<void *>(address + done), n, prot, MAP_SHARED | MAP_FIXED, pool_fd, static_cast<off_t>(at));
        if (p == MAP_FAILED) {
            static int reported = 0;
            if (reported++ < 4) {
                fprintf(stderr, "Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at, n, chunk_fds[c] >= 0 ? "a GPU-visible dma-buf chunk" : "the memfd", strerror(errno));
            }
            return MAP_FAILED;
        }
        done += n;
    }
    return reinterpret_cast<void *>(address);
}

/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk. */
void zero_phys(uint64_t phys, uint64_t size) {
    for (uint64_t done = 0; done < size;) {
        const uint64_t at = phys + done;
        if (!chunk_fds || at >= POOL_SIZE) {
            fallocate(pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(at), static_cast<off_t>(size - done));
            return;
        }
        if (whole_chunks) {
            uint64_t next = 0;
            const Region *r = region_at(at, &next);
            const uint64_t room = r ? r->phys + r->size - at : next - at;
            const uint64_t n = (size - done < room) ? size - done : room;
            if (r && GPU_CHUNK(r->fd)) {
                std::memset(backing_base + at, 0, n);
            } else {
                fallocate(pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(at), static_cast<off_t>(n));
            }
            done += n;
            continue;
        }
        const uint64_t c = at / CHUNK;
        const uint64_t room = c * CHUNK + chunk_size(c) - at;
        const uint64_t n = (size - done < room) ? size - done : room;
        if (GPU_CHUNK(chunk_fds[c])) {
            std::memset(backing_base + at, 0, n);
        } else {
            fallocate(pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(at), static_cast<off_t>(n));
        }
        done += n;
    }
}

void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = (kind == HOOK_MAP) ? hook_map : (kind == HOOK_UNMAP) ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count == sizeof(pending) / sizeof(pending[0])) {
        fputs("STOP: GPU hook queue overflow\n", stderr);
        exit(21);
    }
    pending[pending_count++] = PendingHook{kind, address, size};
}

/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
void flush_hooks() {
    for (;;) {
        write_lock();
        if (!pending_count) {
            write_unlock();
            return;
        }
        PendingHook h = pending[0];
        std::memmove(pending, pending + 1, --pending_count * sizeof(PendingHook));
        GpuRange hook = (h.kind == HOOK_MAP) ? hook_map : (h.kind == HOOK_UNMAP) ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address, h.size);
    }
}

inline int flex_test(uint64_t page) {
    return static_cast<int>((flex_bitmap[page / 64] >> (page % 64)) & 1);
}

void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p = first; p < first + count; ++p) {
        if (used) {
            flex_bitmap[p / 64] |= (UINT64_C(1) << (p % 64));
        } else {
            flex_bitmap[p / 64] &= ~(UINT64_C(1) << (p % 64));
        }
    }
}

uint64_t flex_alloc(uint64_t size) {
    const uint64_t pages = size / PAGE;
    const uint64_t total = FLEX_SPAN / PAGE;
    uint64_t run = 0;
    for (uint64_t p = 0; p < total; ++p) {
        run = flex_test(p) ? 0 : run + 1;
        if (run == pages) {
            flex_set(p + 1 - pages, pages, 1);
            return POOL_SIZE + (p + 1 - pages) * PAGE;
        }
    }
    return std::numeric_limits<uint64_t>::max();
}

void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys - POOL_SIZE) / PAGE, size / PAGE, 0);
    fallocate(pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(phys), static_cast<off_t>(size));
}

size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo = 0;
    size_t hi = vma_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (vmas[mid].end <= a) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int vma_insert(size_t at, Vma v) {
    if (vma_count == vma_capacity) {
        size_t capacity = vma_capacity ? vma_capacity * 2 : 256;
        auto *next = static_cast<Vma *>(realloc(vmas, capacity * sizeof(Vma)));
        if (!next) return -1;
        vmas = next;
        vma_capacity = capacity;
    }
    std::memmove(vmas + at + 1, vmas + at, (vma_count - at) * sizeof(Vma));
    vmas[at] = v;
    ++vma_count;
    return 0;
}

void vma_erase(size_t at, size_t n) {
    std::memmove(vmas + at, vmas + at + n, (vma_count - at - n) * sizeof(Vma));
    vma_count -= n;
}

int split_at(uintptr_t a) {
    size_t i = vma_index(a);
    if (i == vma_count || vmas[i].start >= a) return 0;
    Vma right = vmas[i];
    right.start = a;
    if (right.kind == KIND_DIRECT) right.phys += (a - vmas[i].start);
    vmas[i].end = a;
    return vma_insert(i + 1, right);
}

/* Range [start,end) split on its edges; returns the first index inside it. */
size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}

int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i = vma_index(start);
    uintptr_t at = start;
    for (; at < end; ++i) {
        if (i == vma_count || vmas[i].start > at || (!allow_reserved && vmas[i].kind == KIND_RESERVED)) return 0;
        at = vmas[i].end;
    }
    return 1;
}

int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i = vma_index(start); i < vma_count && vmas[i].start < end; ++i) {
        if (!ignore_reserved || vmas[i].kind != KIND_RESERVED) return 1;
    }
    return 0;
}

/* First-fit search in the PS4 user range, starting from the hint. */
uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at = align_up(hint < USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i = vma_index(at); at + size <= USER_MAX; ++i) {
        if (i == vma_count || vmas[i].start >= at + size) return at;
        at = align_up(vmas[i].end, alignment);
    }
    return 0;
}

void drop_range(uintptr_t start, uintptr_t end) {
    int error = 0;
    size_t i = carve(start, end, &error);
    size_t n = 0;
    while (i + n < vma_count && vmas[i + n].end <= end && vmas[i + n].start >= start) {
        if (vmas[i + n].kind == KIND_FLEXIBLE) {
            flexible_bytes -= (vmas[i + n].end - vmas[i + n].start);
            flex_free(vmas[i + n].phys, vmas[i + n].end - vmas[i + n].start);
        }
        ++n;
    }
    vma_erase(i, n);
}

/* bbport: write traps (runtime_memory_trap, below), kept across the game's own changes. */
void trap_reapply(uintptr_t start, uintptr_t end);
void trap_forget(uintptr_t start, uintptr_t end);

/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
              int kind, int type, uint64_t phys) {
    auto address = reinterpret_cast<uintptr_t>(*inout);
    if (flags & MAP_FIXED_FLAG) {
        if (!address || (address % PAGE)) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address, address + size, 1)) return NO_MEMORY;
    } else {
        address = find_free(address, size, alignment);
        if (!address) return NO_MEMORY;
    }
    void *mapped = (kind != KIND_RESERVED)
                       ? map_phys(address, size, host_prot(prot), phys)
                       : mmap(reinterpret_cast<void *>(address), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED) return NO_MEMORY;
    drop_range(address, address + size);
    trap_forget(address, address + size); /* a new mapping starts without traps */
    if (vma_insert(vma_index(address), Vma{address, address + size, kind, prot, type, phys})) return NO_MEMORY;
    if (kind == KIND_FLEXIBLE) flexible_bytes += size;
    if (kind != KIND_RESERVED) queue_hook(HOOK_MAP, address, size);
    *inout = mapped;
    return 0;
}

int block_type(uint64_t phys) {
    for (int i = 0; i < LIMIT; ++i) {
        if (blocks[i].used && phys >= blocks[i].start && phys - blocks[i].start < blocks[i].size) return blocks[i].type;
    }
    return -1;
}

int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at = phys; at < phys + size;) {
        int found = 0;
        for (int i = 0; i < LIMIT; ++i) {
            if (blocks[i].used && at >= blocks[i].start && at - blocks[i].start < blocks[i].size) {
                at = blocks[i].start + blocks[i].size;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

ABI uint64_t direct_size() {
    return GAME_POOL_SIZE;
}

ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                            uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || (size % PAGE) || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (static_cast<uint64_t>(high) < GAME_POOL_SIZE) ? static_cast<uint64_t>(high) : GAME_POOL_SIZE;
    if (static_cast<uint64_t>(low) >= end || size > end - static_cast<uint64_t>(low)) return NO_SPACE;
    write_lock();
    if (pool()) {
        write_unlock();
        return NO_MEMORY;
    }
    uint64_t start = align_up(static_cast<uint64_t>(low), alignment);
    int slot = -1;
    for (int i = 0; i < LIMIT; ++i) {
        if (!blocks[i].used) {
            slot = i;
            break;
        }
    }
    for (;;) {
        if (slot < 0 || start > end || size > end - start) {
            write_unlock();
            return NO_SPACE;
        }
        int overlap = 0;
        for (int i = 0; i < LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start + b.size && b.start < start + size) {
                start = align_up(b.start + b.size, alignment);
                overlap = 1;
                break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = Block{start, size, type, 1};
    *out = static_cast<int64_t>(start);
    ensure_chunks(start, size);
    ++allocations;
    live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}

int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment);

int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment = PAGE;
    if (!out || physical < 0 || (static_cast<uint64_t>(physical) % PAGE) || !size || (size % PAGE) || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated(static_cast<uint64_t>(physical), size)) return INVALID;
    int32_t result = place(out, size, prot, flags, alignment, KIND_DIRECT, block_type(static_cast<uint64_t>(physical)), static_cast<uint64_t>(physical));
    if (!result) ++maps;
    return result;
}

ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result = map_direct_locked(out, size, prot, flags, physical, alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    else printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", static_cast<uint64_t>(physical), size, static_cast<unsigned>(result));
    return result;
}

ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                             uint64_t alignment, const char *name) {
    (void)name;
    return direct_map(out, size, prot, flags, physical, alignment);
}

int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if ((start % PAGE) || !size) return INVALID;
    uint64_t end = start + align_up(size, PAGE);
    for (size_t i = vma_index(start); i < vma_count && vmas[i].start < end; ++i) {
        if (vmas[i].kind != KIND_RESERVED) {
            uintptr_t a = (vmas[i].start > start) ? vmas[i].start : start;
            uintptr_t b = (vmas[i].end < end) ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP, a, b - a);
        }
    }
    for (size_t i = vma_index(start); i < vma_count && vmas[i].start < end; ++i) {
        uintptr_t a = (vmas[i].start > start) ? vmas[i].start : start;
        uintptr_t b = (vmas[i].end < end) ? vmas[i].end : end;
        if (munmap(reinterpret_cast<void *>(a), b - a)) return INVALID;
    }
    drop_range(start, end);
    trap_forget(start, end);
    return 0;
}

ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !(reinterpret_cast<uintptr_t>(address) % PAGE) && size) {
        hook_unmap(reinterpret_cast<uintptr_t>(address), align_up(size, PAGE));
    }
    write_lock();
    size_t before = pending_count;
    int32_t result = unmap_locked(reinterpret_cast<uintptr_t>(address), size);
    pending_count = before; /* already notified above */
    write_unlock();
    return result;
}

ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if ((start % PAGE) || (size % PAGE)) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start, size)) {
        write_unlock();
        return INVALID;
    }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i = 0; i < vma_count;) {
        Vma v = vmas[i];
        uint64_t length = v.end - v.start;
        if (v.kind == KIND_DIRECT && v.phys < start + size && start < v.phys + length) {
            uint64_t a = (v.phys > start) ? v.phys : start;
            uint64_t b = (v.phys + length < start + size) ? v.phys + length : start + size;
            unmap_locked(v.start + (a - v.phys), b - a);
            i = vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i = 0; i < LIMIT; ++i) {
        Block *b = &blocks[i];
        if (!b->used || b->start >= start + size || start >= b->start + b->size) continue;
        uint64_t a = (b->start > start) ? b->start : start;
        uint64_t e = (b->start + b->size < start + size) ? b->start + b->size : start + size;
        Block left = *b;
        Block right = *b;
        left.size = a - b->start;
        right.start = e;
        right.size = b->start + b->size - e;
        b->used = 0;
        if (left.size) *b = left;
        if (right.size) {
            for (int j = 0; j < LIMIT; ++j) {
                if (!blocks[j].used) {
                    blocks[j] = right;
                    break;
                }
            }
        }
        live_bytes -= (e - a);
    }
    zero_phys(start, size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}

int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || (size % PAGE) || (prot & ~0x37)) return INVALID;
    if (flexible_bytes + size > FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys = flex_alloc(size);
    if (phys == std::numeric_limits<uint64_t>::max()) return NO_MEMORY;
    int32_t result = place(inout, size, prot, flags, PAGE, KIND_FLEXIBLE, 0, phys);
    if (result) flex_free(phys, size);
    else ++flexible_maps;
    return result;
}

ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result = map_flexible_locked(inout, size, prot, flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr, "Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, static_cast<unsigned>(result), flexible_bytes);
    return result;
}

ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name;
    return map_flexible(inout, size, prot, flags);
}

ABI int32_t release_flexible(void *address, uint64_t size) {
    return direct_unmap(address, size);
}

/* POSIX mmap: the game's libc grows its malloc heap with anonymous private mappings once the
 * heap it started with is full (seen after minutes of area streaming). They come from flexible
 * memory as sceKernelMapFlexibleMemory does; files are not mapped. MAP_FAILED (-1) and errno on
 * failure, as POSIX. */
std::atomic<uint64_t> heap_growths{0};

ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    heap_growths.fetch_add(1, std::memory_order_relaxed);
    enum { MAP_ANON_FLAG = 0x1000, POSIX_ENOMEM = 12, POSIX_ENODEV = 19, POSIX_EINVAL = 22 };
    if (!(flags & MAP_ANON_FLAG) || fd != -1 || offset) {
        *runtime_errno() = POSIX_ENODEV;
        return reinterpret_cast<void *>(-1);
    }
    if (!size) {
        *runtime_errno() = POSIX_EINVAL;
        return reinterpret_cast<void *>(-1);
    }
    void *out = (flags & MAP_FIXED_FLAG) ? addr : nullptr;
    const int32_t result = map_flexible(&out, align_up(size, PAGE), prot & 0x37, flags & MAP_FIXED_FLAG);
    uint64_t sites[3];
    runtime_guest_call_sites(sites);
    printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
           size, sites[0], sites[1], sites[2]);
    if (result) {
        *runtime_errno() = (result == NO_MEMORY) ? POSIX_ENOMEM : POSIX_EINVAL;
        return reinterpret_cast<void *>(-1);
    }
    return out;
}

ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment = PAGE;
    if (!inout || !size || (size % PAGE) || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps(reinterpret_cast<uintptr_t>(*inout), reinterpret_cast<uintptr_t>(*inout) + size, 0)) {
        write_unlock();
        return NO_MEMORY;
    }
    int32_t result = place(inout, size, 0, flags, alignment, KIND_RESERVED, 0, 0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}

int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end = start + align_up(size, PAGE);
    start &= ~(static_cast<uintptr_t>(PAGE - 1));
    if ((prot & ~0x37) || !covered(start, end, 0)) return INVALID;
    if (mprotect(reinterpret_cast<void *>(start), end - start, host_prot(prot))) return INVALID;
    trap_reapply(start, end); /* pages with write traps stay read-only */
    int error = 0;
    size_t i = carve(start, end, &error);
    if (error) return NO_MEMORY;
    for (; i < vma_count && vmas[i].start < end; ++i) {
        vmas[i].prot = prot;
        if (type >= 0) vmas[i].type = type;
    }
    queue_hook(HOOK_INVALIDATE, start, end - start);
    ++protects;
    return 0;
}

ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result = protect_locked(reinterpret_cast<uintptr_t>(address), size, prot, -1);
    write_unlock();
    flush_hooks();
    return result;
}

ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i = vma_index(reinterpret_cast<uintptr_t>(address));
    if (i == vma_count || vmas[i].start > reinterpret_cast<uintptr_t>(address) || vmas[i].kind == KIND_RESERVED) {
        read_unlock();
        return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first = i;
    size_t last = i;
    while (first && vmas[first - 1].end == vmas[first].start && vmas[first - 1].kind != KIND_RESERVED && vmas[first - 1].prot == vmas[i].prot) --first;
    while (last + 1 < vma_count && vmas[last + 1].start == vmas[last].end && vmas[last + 1].kind != KIND_RESERVED && vmas[last + 1].prot == vmas[i].prot) ++last;
    if (start) *start = reinterpret_cast<void *>(vmas[first].start);
    if (end) *end = reinterpret_cast<void *>(vmas[last].end);
    if (prot) *prot = static_cast<uint32_t>(vmas[i].prot);
    __atomic_add_fetch(&queries, 1, __ATOMIC_RELAXED);
    read_unlock();
    return 0;
}

ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size != sizeof(*info)) return INVALID;
    read_lock();
    size_t i = vma_index(reinterpret_cast<uintptr_t>(address));
    if (i == vma_count || (vmas[i].start > reinterpret_cast<uintptr_t>(address) && !(flags & 1))) {
        read_unlock();
        return ACCESS;
    }
    Vma v = vmas[i];
    std::memset(info, 0, sizeof(*info));
    info->start = v.start;
    info->end = v.end;
    info->offset = (v.kind == KIND_DIRECT) ? v.phys : 0;
    info->protection = v.prot;
    info->memory_type = v.type;
    info->flags = (v.kind == KIND_FLEXIBLE ? 1u : 0) | (v.kind == KIND_DIRECT ? 2u : 0) | (v.kind != KIND_RESERVED ? 16u : 0);
    strcpy(info->name, (v.kind == KIND_RESERVED) ? "reserved" : (v.kind == KIND_DIRECT) ? "direct" : "flexible");
    __atomic_add_fetch(&queries, 1, __ATOMIC_RELAXED);
    read_unlock();
    return 0;
}

ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i = 0; i < LIMIT; ++i) {
        if (blocks[i].used && phys >= blocks[i].start && phys - blocks[i].start < blocks[i].size) {
            if (type) *type = blocks[i].type;
            if (start) *start = reinterpret_cast<void *>(static_cast<uintptr_t>(blocks[i].start));
            if (end) *end = reinterpret_cast<void *>(static_cast<uintptr_t>(blocks[i].start + blocks[i].size));
            read_unlock();
            return 0;
        }
    }
    read_unlock();
    return ACCESS;
}

/* The direct memory allocation containing `offset` (or, flags & 1, the next one after it). */
struct DirectQueryInfo {
    int64_t start{0};
    int64_t end{0};
    int32_t memory_type{0};
};

ABI int32_t direct_query(int64_t offset, int flags, DirectQueryInfo *info, uint64_t size) {
    if (!info || size < sizeof(DirectQueryInfo) || offset < 0) return INVALID;
    read_lock();
    int best = -1;
    for (int i = 0; i < LIMIT; ++i) {
        if (!blocks[i].used) continue;
        if (blocks[i].start >= GAME_POOL_SIZE) continue; /* the port's own (runtime_extra_direct_map) */
        const auto at = static_cast<uint64_t>(offset);
        if (at >= blocks[i].start && at - blocks[i].start < blocks[i].size) {
            best = i;
            break;
        }
        if ((flags & 1) && blocks[i].start > at && (best < 0 || blocks[i].start < blocks[best].start)) {
            best = i;
        }
    }
    if (best >= 0) {
        info->start = static_cast<int64_t>(blocks[best].start);
        info->end = static_cast<int64_t>(blocks[best].start + blocks[best].size);
        info->memory_type = blocks[best].type;
    }
    read_unlock();
    return best >= 0 ? 0 : static_cast<int32_t>(UINT32_C(0x80020002)); /* ENOENT */
}

ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count < 0) return INVALID;
    int32_t result = 0;
    int done = 0;
    write_lock();
    for (; done < count; ++done) {
        BatchEntry *e = &entries[done];
        switch (e->operation) {
        case 0:
            result = map_direct_locked(&e->start, e->length, static_cast<uint8_t>(e->prot), flags, static_cast<int64_t>(e->offset), 0);
            break;
        case 1:
            result = unmap_locked(reinterpret_cast<uintptr_t>(e->start), e->length);
            break;
        case 2:
            result = protect_locked(reinterpret_cast<uintptr_t>(e->start), e->length, static_cast<uint8_t>(e->prot), -1);
            break;
        case 3:
            result = map_flexible_locked(&e->start, e->length, static_cast<uint8_t>(e->prot), flags);
            break;
        case 4:
            result = protect_locked(reinterpret_cast<uintptr_t>(e->start), e->length, static_cast<uint8_t>(e->prot), e->type);
            break;
        default:
            result = INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed = done;
    return result;
}

ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries, count, processed, MAP_FIXED_FLAG);
}

static const RuntimeExport exports[] = {
    {"sceKernelGetDirectMemorySize", reinterpret_cast<void *>(direct_size)},
    {"sceKernelAllocateDirectMemory", reinterpret_cast<void *>(direct_allocate)},
    {"sceKernelMapDirectMemory", reinterpret_cast<void *>(direct_map)},
    {"sceKernelMapNamedDirectMemory", reinterpret_cast<void *>(direct_map_named)},
    {"sceKernelReleaseDirectMemory", reinterpret_cast<void *>(direct_release)},
    {"sceKernelMunmap", reinterpret_cast<void *>(direct_unmap)},
    {"munmap", reinterpret_cast<void *>(direct_unmap)},
    {"sceKernelMapFlexibleMemory", reinterpret_cast<void *>(map_flexible)},
    {"sceKernelMapNamedFlexibleMemory", reinterpret_cast<void *>(map_flexible_named)},
    {"sceKernelReleaseFlexibleMemory", reinterpret_cast<void *>(release_flexible)},
    {"sceKernelReserveVirtualRange", reinterpret_cast<void *>(reserve_range)},
    {"sceKernelMprotect", reinterpret_cast<void *>(kernel_mprotect)},
    {"sceKernelQueryMemoryProtection", reinterpret_cast<void *>(query_protection)},
    {"sceKernelVirtualQuery", reinterpret_cast<void *>(virtual_query)},
    {"sceKernelGetDirectMemoryType", reinterpret_cast<void *>(direct_memory_type)},
    {"sceKernelDirectMemoryQuery", reinterpret_cast<void *>(direct_query)},
    {"sceKernelBatchMap", reinterpret_cast<void *>(batch_map)},
    {"sceKernelBatchMap2", reinterpret_cast<void *>(batch_map2)},
    {"mmap", reinterpret_cast<void *>(posix_mmap)},
    {"pO96TwzOm5E", reinterpret_cast<void *>(direct_size)},
    {"rTXw65xmLIA", reinterpret_cast<void *>(direct_allocate)},
    {"L-Q3LEjIbgA", reinterpret_cast<void *>(direct_map)},
    {"MBuItvba6z8", reinterpret_cast<void *>(direct_release)},
    {"cQke9UuBQOk", reinterpret_cast<void *>(direct_unmap)},
};

constexpr uint64_t LOW_MIN = UINT64_C(0x0800000000);
uintptr_t low_next = LOW_MIN;

static void *toggle_watcher(void *path) {
    for (unsigned long long last = ULLONG_MAX;;) {
        FILE *f = fopen(static_cast<const char *>(path), "r");
        unsigned long long value = 0;
        unsigned long long experiment = 0;
        if (f) {
            if (fscanf(f, "%llu %llu", &value, &experiment) < 1) value = 0;
            fclose(f);
        }
        __atomic_store_n(&runtime_experiment_bits, static_cast<uint64_t>(experiment), __ATOMIC_RELEASE);
        if (value != last) {
            __atomic_store_n(&runtime_disabled_optimizations, static_cast<uint64_t>(value), __ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n", value);
            last = value;
        }
        struct timespec t = {0, 250000000};
        nanosleep(&t, nullptr);
    }
    return nullptr;
}

constexpr uint64_t WATCH_LIMIT = (1ULL << 40);
std::atomic<uint64_t> *write_watch = nullptr;
GpuRange hook_cpu_write = nullptr;

constexpr uint64_t TRAP_LIMIT = (1ULL << 40);
constexpr uint8_t TRAP_NO_ACCESS = 0xf0u;
uint8_t *trap_reasons = nullptr; /* one byte per 4 KiB page below TRAP_LIMIT (reserved, not committed) */
pthread_mutex_t trap_mutex = PTHREAD_MUTEX_INITIALIZER;

/* A page's trap level: 0 none, 1 read-only, 2 no access. */
inline int trap_level(uint8_t reasons) {
    return !reasons ? 0 : (reasons & TRAP_NO_ACCESS) ? 2 : 1;
}

/* The pages [a, b) to the game's protection, without write at level 1, without access at 2
 * (runtime lock held). */
void trap_apply(uintptr_t a, uintptr_t b, int level) {
    for (size_t i = vma_index(a); i < vma_count && vmas[i].start < b; ++i) {
        if (vmas[i].kind == KIND_RESERVED) continue;
        const uintptr_t s = (vmas[i].start > a) ? vmas[i].start : a;
        const uintptr_t e = (vmas[i].end < b) ? vmas[i].end : b;
        const int prot = host_prot(vmas[i].prot);
        mprotect(reinterpret_cast<void *>(s), e - s, (level == 2) ? PROT_NONE : level ? (prot & ~PROT_WRITE) : prot);
    }
}

/* No traps in [start, end) any more: a mapping replaced or gone (runtime lock held). */
void trap_forget(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start >= TRAP_LIMIT) return;
    if (end > TRAP_LIMIT) end = TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    std::memset(trap_reasons + (start >> 12), 0, ((end + 4095) >> 12) - (start >> 12));
    pthread_mutex_unlock(&trap_mutex);
}

/* Pages with traps in [start, end) read-only (or no access) again after the game changed their
 * protection (runtime lock held). */
void trap_reapply(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start >= TRAP_LIMIT) return;
    if (end > TRAP_LIMIT) end = TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    uintptr_t run = 0;
    int run_level = 0;
    for (uintptr_t p = (start >> 12); p < ((end + 4095) >> 12); ++p) {
        const int level = trap_level(trap_reasons[p]);
        if (run && level != run_level) {
            trap_apply(run, p << 12, run_level);
            run = 0;
        }
        if (level && !run) {
            run = p << 12;
            run_level = level;
        }
    }
    if (run) trap_apply(run, end, run_level);
    pthread_mutex_unlock(&trap_mutex);
}

/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) {
        std::memcpy(dst, src, n);
        return;
    }
    while (n) {
        const auto at = reinterpret_cast<uintptr_t>(dst);
        if (n >= 8 && !(at & 7)) {
            uint64_t v = 0;
            std::memcpy(&v, src, 8);
            __atomic_store_n(reinterpret_cast<uint64_t *>(dst), v, __ATOMIC_RELEASE);
            dst += 8;
            src += 8;
            n -= 8;
        } else if (n >= 4 && !(at & 3)) {
            uint32_t v = 0;
            std::memcpy(&v, src, 4);
            __atomic_store_n(reinterpret_cast<uint32_t *>(dst), v, __ATOMIC_RELEASE);
            dst += 4;
            src += 4;
            n -= 4;
        } else if (n >= 2 && !(at & 1)) {
            uint16_t v = 0;
            std::memcpy(&v, src, 2);
            __atomic_store_n(reinterpret_cast<uint16_t *>(dst), v, __ATOMIC_RELEASE);
            dst += 2;
            src += 2;
            n -= 2;
        } else {
            __atomic_store_n(dst, *src, __ATOMIC_RELEASE);
            ++dst;
            ++src;
            --n;
        }
    }
}

/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
struct CachedRegion {
    uint64_t generation{0};
    uintptr_t start{0};
    uintptr_t end{0};
    int kind{0};
};

__thread CachedRegion region_cache[8];
__thread unsigned region_cache_next = 0;

const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation = __atomic_load_n(&table_generation, __ATOMIC_ACQUIRE);
    if (generation & 1) return nullptr;
    for (unsigned i = 0; i < 8; ++i) {
        const CachedRegion *c = &region_cache[i];
        if (c->generation == generation && c->end && c->start <= address && address < c->end) return c;
    }
    return nullptr;
}

/* Called under the read lock after a lookup of address. */
void cache_region(size_t i) {
    uint64_t generation = __atomic_load_n(&table_generation, __ATOMIC_ACQUIRE);
    if (i == vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8] = CachedRegion{generation, vmas[i].start, vmas[i].end, vmas[i].kind};
}

} // namespace runtime::memory

extern "C" {

using namespace runtime::memory;

uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
__thread sigjmp_buf *runtime_fault_recover = nullptr;

void runtime_memory_register() {
    runtime_register_exports(exports, sizeof(exports) / sizeof(exports[0]));
}

uintptr_t runtime_memory_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

/* The guest's address range, reserved before anything else can land there (Windows: one
 * placeholder the game's mappings replace in; Linux needs nothing). */
void runtime_memory_reserve() {
#ifdef _WIN32
    if (bb_reserve_arena(USER_MIN, USER_MAX)) exit(21);
#endif
}

void *runtime_low_map(size_t size, int prot) {
#ifdef _WIN32
    constexpr uint64_t granule = UINT64_C(65536); /* VirtualAlloc places allocations at 64 KiB */
#else
    constexpr uint64_t granule = PAGE;
#endif
    size = align_up(size, PAGE);
    write_lock();
    void *p = MAP_FAILED;
    low_next = align_up(low_next, granule);
    while (low_next + size <= USER_MIN) {
        p = mmap(reinterpret_cast<void *>(low_next), size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        low_next = align_up(low_next + size + PAGE, granule); /* unmapped gap catches overruns */
        if (p != MAP_FAILED) break;
    }
    write_unlock();
    return (p == MAP_FAILED) ? nullptr : p;
}

uint64_t runtime_heap_growths() {
    return heap_growths.load(std::memory_order_relaxed);
}

/* bbport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) {
    whole_chunks = whole;
}

/* bbport: the host address of direct memory phys in the backing view (host memory import). */
void *runtime_memory_backing_pointer(uint64_t phys) {
    return backing_base ? (backing_base + phys) : nullptr;
}

void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size)) {
    write_lock();
    chunk_alloc = alloc;
    write_unlock();
}

void runtime_memory_set_cpu_write_hook(GpuRange hook) {
    if (!write_watch) {
        void *bits = mmap(nullptr, WATCH_LIMIT >> 15, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (bits == MAP_FAILED) {
            perror("bbport: write watch bitmap");
            return;
        }
        write_watch = static_cast<std::atomic<uint64_t> *>(bits);
    }
    hook_cpu_write = hook;
}

void runtime_memory_set_coherence_hooks(const RuntimeMemoryCoherenceHooks *hooks) {
    if (!hooks) return;
    const char *toggles = getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) {
        pthread_create(&watcher, nullptr, toggle_watcher, const_cast<char *>(toggles));
    }
    write_lock();
    hook_map = hooks->map;
    hook_unmap = hooks->unmap;
    hook_invalidate = hooks->invalidate;
    hook_note_write = hooks->note_write;
    for (size_t i = 0; i < vma_count; ++i) {
        if (vmas[i].kind != KIND_RESERVED) {
            queue_hook(HOOK_MAP, vmas[i].start, vmas[i].end - vmas[i].start);
        }
    }
    write_unlock();
    flush_hooks();
    if (hooks->cpu_write) {
        runtime_memory_set_cpu_write_hook(hooks->cpu_write);
    }
}

void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    RuntimeMemoryCoherenceHooks h = {
        .map = map,
        .unmap = unmap,
        .invalidate = invalidate,
        .note_write = hook_note_write,
        .cpu_write = hook_cpu_write
    };
    runtime_memory_set_coherence_hooks(&h);
}

void runtime_memory_set_note_write_hook(GpuRange note) {
    hook_note_write = note;
}

void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    if (!hook_invalidate || !size) return;
    uintptr_t starts[8];
    uintptr_t ends[8];
    int n = 0;
    read_lock();
    for (size_t i = vma_index(address); i < vma_count && vmas[i].start < address + size && n < 8; ++i) {
        if (vmas[i].kind == KIND_RESERVED || !(vmas[i].prot & 0x30)) continue;
        starts[n] = (vmas[i].start > address) ? vmas[i].start : address;
        ends[n] = (vmas[i].end < address + size) ? vmas[i].end : (address + size);
        ++n;
    }
    read_unlock();
    GpuRange hook = hook_note_write ? hook_note_write : hook_invalidate;
    for (int i = 0; i < n; ++i) hook(starts[i], ends[i] - starts[i]);
}

void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch) {
    if (!write_watch || !size || address >= WATCH_LIMIT) return;
    uint64_t first = address >> 12;
    uint64_t last = (address + size - 1) >> 12;
    if (last >= (WATCH_LIMIT >> 12)) last = (WATCH_LIMIT >> 12) - 1;
    for (uint64_t word = (first >> 6); word <= (last >> 6); ++word) {
        const uint64_t lo = (word == (first >> 6)) ? (first & 63) : 0;
        const uint64_t hi = (word == (last >> 6)) ? (last & 63) : 63;
        const uint64_t mask = ((hi == 63) ? ~0ULL : (2ULL << hi) - 1) & ~((1ULL << lo) - 1);
        if (watch) {
            write_watch[word].fetch_or(mask, std::memory_order_release);
        } else {
            write_watch[word].fetch_and(~mask, std::memory_order_release);
        }
    }
}

void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    if (!write_watch || !size || address >= WATCH_LIMIT || address + size > WATCH_LIMIT) return;
    const uint64_t first = address >> 12;
    const uint64_t last = (address + size - 1) >> 12;
    for (uint64_t word = (first >> 6); word <= (last >> 6); ++word) {
        const uint64_t lo = (word == (first >> 6)) ? (first & 63) : 0;
        const uint64_t hi = (word == (last >> 6)) ? (last & 63) : 63;
        const uint64_t mask = ((hi == 63) ? ~0ULL : (2ULL << hi) - 1) & ~((1ULL << lo) - 1);
        if (write_watch[word].load(std::memory_order_acquire) & mask) {
            hook_cpu_write(address, size);
            return;
        }
    }
}

void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on) {
    if (!size || address >= TRAP_LIMIT) return;
    if (!trap_reasons) {
        void *bytes = mmap(nullptr, TRAP_LIMIT >> 12, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (bytes == MAP_FAILED) {
            perror("bbport: write trap table");
            return;
        }
        uint8_t *expected = nullptr;
        if (!__atomic_compare_exchange_n(&trap_reasons, &expected, static_cast<uint8_t *>(bytes), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            munmap(bytes, TRAP_LIMIT >> 12);
        }
    }
    uintptr_t end = (address + size > TRAP_LIMIT) ? TRAP_LIMIT : (address + size);
    read_lock();
    pthread_mutex_lock(&trap_mutex);
    /* Runs of pages whose trap level changes, one mprotect each. */
    uintptr_t run = 0;
    int run_level = 0;
    const uintptr_t first = address >> 12;
    const uintptr_t last = (end - 1) >> 12;
    for (uintptr_t p = first; p <= last; ++p) {
        const uint8_t old = trap_reasons[p];
        const auto now = on ? static_cast<uint8_t>(old | reason) : static_cast<uint8_t>(old & ~reason);
        if (old != now) trap_reasons[p] = now;
        const int level = trap_level(now);
        const int change = (trap_level(old) != level);
        if (run && (!change || level != run_level)) {
            trap_apply(run, p << 12, run_level);
            run = 0;
        }
        if (change && !run) {
            run = p << 12;
            run_level = level;
        }
    }
    if (run) trap_apply(run, (last + 1) << 12, run_level);
    pthread_mutex_unlock(&trap_mutex);
    read_unlock();
}

void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size) {
    const uint8_t *table = __atomic_load_n(&trap_reasons, __ATOMIC_ACQUIRE);
    if (!table || !size || address >= TRAP_LIMIT || !hook_cpu_write) return;
    uint64_t last = (address + size - 1) >> 12;
    if (last >= (TRAP_LIMIT >> 12)) last = (TRAP_LIMIT >> 12) - 1;
    for (uint64_t p = (address >> 12); p <= last; ++p) {
        if (__atomic_load_n(&table[p], __ATOMIC_RELAXED)) {
            hook_cpu_write(address, size);
            return;
        }
    }
}

unsigned int runtime_memory_trap_reasons(uintptr_t address) {
    const uint8_t *table = __atomic_load_n(&trap_reasons, __ATOMIC_ACQUIRE);
    return (table && address < TRAP_LIMIT) ? __atomic_load_n(&table[address >> 12], __ATOMIC_ACQUIRE) : 0;
}

int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end) {
    read_lock();
    const size_t i = vma_index(address);
    const int ok = (i < vma_count && vmas[i].start <= address && address < vmas[i].end && vmas[i].kind == KIND_DIRECT);
    if (ok) {
        *phys = vmas[i].phys + (address - vmas[i].start);
        *end = vmas[i].end;
    }
    read_unlock();
    return ok;
}

int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    read_lock();
    const size_t i = vma_index(address);
    const int ok = (i < vma_count && vmas[i].start <= address && vmas[i].kind != KIND_RESERVED);
    if (ok) {
        *prot = vmas[i].prot;
        *type = (vmas[i].kind == KIND_DIRECT) ? vmas[i].type : -1;
        *end = vmas[i].end;
    }
    read_unlock();
    return ok;
}

int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok = 1;
    for (uintptr_t at = address, end = address + size; at < end && ok;) {
        size_t i = vma_index(at);
        if (i == vma_count || vmas[i].start > at || vmas[i].kind == KIND_RESERVED) {
            ok = 0;
            break;
        }
        uint64_t n = ((vmas[i].end < end) ? vmas[i].end : end) - at;
        store_once(backing_base + vmas[i].phys + (at - vmas[i].start),
                   reinterpret_cast<const unsigned char *>(data) + (at - address), n);
        at += n;
    }
    read_unlock();
    return ok;
}

void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    auto *out = static_cast<unsigned char *>(data);
    read_lock();
    for (uintptr_t at = address, end = address + size; at < end;) {
        const size_t i = vma_index(at);
        uint64_t n = 0;
        if (i < vma_count && vmas[i].start <= at && vmas[i].kind != KIND_RESERVED) {
            n = ((vmas[i].end < end) ? vmas[i].end : end) - at;
            std::memcpy(out + (at - address), backing_base + vmas[i].phys + (at - vmas[i].start), n);
        } else {
            const uintptr_t next = (i == vma_count) ? end : (vmas[i].start > at) ? vmas[i].start : vmas[i].end;
            n = ((next < end) ? next : end) - at;
            std::memset(out + (at - address), 0, n);
        }
        at += n;
    }
    read_unlock();
}

const uint64_t *runtime_memory_generation() {
    return &table_generation;
}

uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c = (runtime_disabled_optimizations & 1) ? nullptr : cached_region(address);
    if (c && c->kind != KIND_RESERVED && size <= c->end - address) return size;
    read_lock();
    {
        size_t i = vma_index(address);
        if (i < vma_count && vmas[i].start <= address) cache_region(i);
    }
    uint64_t length = 0;
    for (size_t i = vma_index(address); i < vma_count && length < size; ++i) {
        if (vmas[i].start > address + length || vmas[i].kind == KIND_RESERVED) break;
        length = vmas[i].end - address;
    }
    read_unlock();
    return (length < size) ? length : size;
}

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c = (runtime_disabled_optimizations & 1) ? nullptr : cached_region(address);
    if (c) {
        *start = c->start;
        *end = c->end;
        *mapped = (c->kind != KIND_RESERVED);
        return 1;
    }
    read_lock();
    size_t i = vma_index(address);
    int found = (i < vma_count);
    if (found && vmas[i].start <= address) {
        cache_region(i);
        *start = vmas[i].start;
        *end = vmas[i].end;
        *mapped = (vmas[i].kind != KIND_RESERVED);
    } else if (found) {
        *start = i ? vmas[i - 1].end : 0;
        *end = vmas[i].start;
        *mapped = 0;
    }
    read_unlock();
    return found;
}

void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i = vma_index(address); i < vma_count && vmas[i].start < address + size; ++i) {
        if (vmas[i].kind == KIND_RESERVED) continue;
        uintptr_t a = (vmas[i].start > address) ? vmas[i].start : address;
        uintptr_t b = (vmas[i].end < address + size) ? vmas[i].end : (address + size);
        int guest = host_prot(vmas[i].prot);
        int want = (read ? PROT_READ : 0) | (write ? (PROT_WRITE | PROT_READ) : 0) | (guest & PROT_EXEC);
        mprotect(reinterpret_cast<void *>(a), b - a, guest & want);
    }
    read_unlock();
}

int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c = (runtime_disabled_optimizations & 1) ? nullptr : cached_region(address);
    if (c && c->kind != KIND_RESERVED && size <= c->end - address) return 1;
    read_lock();
    int result = covered(address, address + size, 0);
    read_unlock();
    return result;
}

int runtime_memory_check_mapped(uintptr_t address, uint64_t size) {
    if (!size) return 1;
    const CachedRegion *c = (runtime_disabled_optimizations & 1) ? nullptr : cached_region(address);
    if (c && c->kind != KIND_RESERVED && address >= c->start && address + size <= c->end) return 1;
    uintptr_t start = 0;
    uintptr_t end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) return 0;
    return (address + size <= end);
}

void runtime_memory_report() {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}

int runtime_extra_direct_map(uint64_t size, uint64_t alignment, void **out) {
    if (!out || !size || (size % PAGE) || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if (pool()) {
        write_unlock();
        return NO_MEMORY;
    }
    const uint64_t end = POOL_SIZE;
    uint64_t start = align_up(GAME_POOL_SIZE, alignment);
    int slot = -1;
    for (int i = 0; i < LIMIT; ++i) {
        if (!blocks[i].used) {
            slot = i;
            break;
        }
    }
    for (;;) {
        if (slot < 0 || start > end || size > end - start) {
            write_unlock();
            return NO_SPACE;
        }
        int overlap = 0;
        for (int i = 0; i < LIMIT; ++i) {
            const Block b = blocks[i];
            if (b.used && start < b.start + b.size && b.start < start + size) {
                start = align_up(b.start + b.size, alignment);
                overlap = 1;
                break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = Block{start, size, 3, 1};
    ensure_chunks(start, size);
    *out = nullptr;
    const int32_t result = map_direct_locked(out, size, 0x33, 0, static_cast<int64_t>(start), alignment);
    if (result) blocks[slot].used = 0;
    write_unlock();
    flush_hooks();
    if (!result) {
        printf("Runtime: port direct memory %p, size=%" PRIu64 " (past the game's %" PRIu64 " MiB)\n",
               *out, size, GAME_POOL_SIZE >> 20);
    }
    return result;
}

} // extern "C"

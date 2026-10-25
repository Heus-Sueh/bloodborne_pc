#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime_file.hpp"
#include "runtime.h"
#include "runtime_vfs.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR(n) ORBIS_ERR(n)
#define MAX_FILES 1024
#define MAX_MOUNTS 64 /* app0, 16 save data slots, 16 add-ons and the rest */

namespace runtime::file {

struct GuestTimespec {
    int64_t sec, nsec;
};

struct GuestStat {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
};
static_assert(sizeof(GuestStat) == 120, "FreeBSD stat layout");

struct Listing {
    char *names;
    size_t count;
    size_t *offsets;
    unsigned char *types;
};

/* commit: a save file opened for writing is written to a temporary copy (temp), which replaces
 * the file (commit) in one rename when closed. */
struct File {
    int used, host, dirty;
    Listing *dir;
    size_t position;
    char path[512];
    char *commit, *temp;
};

struct Mount {
    char guest[64];
    char host[512];
};

File s_files[MAX_FILES]{};
Mount s_mounts[MAX_MOUNTS]{};
size_t s_mount_count = 0;
std::atomic<size_t> s_opens{0};
std::atomic<size_t> s_reads{0};
std::atomic<size_t> s_writes{0};
std::atomic<size_t> s_missing{0};
std::atomic<uint64_t> s_bytes_read{0};
std::atomic<unsigned> s_temp_serial{0};

std::recursive_mutex s_lock;
char s_user_root[512] = "user";

bool IsSavePath(const char *p) {
    return p && !std::strncmp(p, "/savedata", 9);
}

bool IsTempName(const char *name) {
    const size_t n = std::strlen(name);
    return n > 6 && !std::strcmp(name + n - 6, ".bbtmp");
}

/* Copies left by a crash (none of them open): removed when their directory is mounted. */
void RemoveStaleTemps(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e = nullptr; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        if (static_cast<size_t>(std::snprintf(path, sizeof(path), "%s/%s", dir, e->d_name)) >= sizeof(path)) {
            continue;
        }
        if (IsTempName(e->d_name)) {
            int open_now = 0;
            for (int i = 3; i < MAX_FILES; ++i) {
                if (s_files[i].used && s_files[i].temp && !std::strcmp(s_files[i].temp, path)) {
                    open_now = 1;
                }
            }
            if (!open_now && !unlink(path)) {
                std::printf(
                    "Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",
                    path);
            }
        } else if (depth < 4 && e->d_type == DT_DIR) {
            RemoveStaleTemps(path, depth + 1);
        }
    }
    closedir(d);
}

int MountGuest(const char *guest, const char *host) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    for (size_t i = 0; i < s_mount_count; ++i) {
        if (!std::strcmp(s_mounts[i].guest, guest)) {
            std::snprintf(s_mounts[i].host, sizeof(s_mounts[i].host), "%s", host);
            return 0;
        }
    }
    if (s_mount_count == MAX_MOUNTS || std::strlen(guest) >= 64 || std::strlen(host) >= 512) {
        return -1;
    }
    std::snprintf(s_mounts[s_mount_count].guest, 64, "%s", guest);
    std::snprintf(s_mounts[s_mount_count].host, 512, "%s", host);
    ++s_mount_count;
    if (IsSavePath(guest)) {
        RemoveStaleTemps(host, 0);
    }
    return 0;
}

void UnmountGuest(const char *guest) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    for (size_t i = 0; i < s_mount_count; ++i) {
        if (!std::strcmp(s_mounts[i].guest, guest)) {
            s_mounts[i] = s_mounts[--s_mount_count];
            break;
        }
    }
}

const char *GetUserDir() {
    return s_user_root;
}

void Configure(const char *app0, const char *user) {
    char path[600];
    std::snprintf(s_user_root, sizeof(s_user_root), "%s", user);
    MountGuest("/app0", app0);
    MountGuest("/hostapp", app0);

    bb_vfs_init();
    bb_vfs_mount(VFS_LAYER_BASE, app0);
    const char *mods_enabled = std::getenv("BB_MODS_ENABLED");
    if (!mods_enabled || std::strcmp(mods_enabled, "0") != 0) {
        const char *mods_dir = std::getenv("BB_MODS_DIR");
        const char *mods_cfg = std::getenv("BB_MODS_CONFIG");
        bb_vfs_load_mods(mods_dir ? mods_dir : "mods", mods_cfg);

        char legacy[1024];
        std::snprintf(legacy, sizeof(legacy), "%s-mods", app0);
        bb_vfs_mount_mod(legacy, "legacy-mods");
    }

    const char *writable[] = {"temp0", "download0", "data"};
    mkdir(user, 0755);
    for (int i = 0; i < 3; ++i) {
        std::snprintf(path, sizeof(path), "%s/%s", user, writable[i]);
        mkdir(path, 0755);
        char guest[32];
        std::snprintf(guest, sizeof(guest), "/%s", writable[i]);
        MountGuest(guest, path);
    }
}

/* Resolve a guest path to a host path; returns 0 or a host errno. */
int Translate(const char *guest, char *out, size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    if (guest[0] != '/') {
        std::snprintf(buffer, sizeof(buffer), "/app0/%s", guest);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s", guest);
    }
    for (const char *p = buffer; (p = std::strstr(p, "..")); p += 2) {
        if ((p == buffer || p[-1] == '/') && (p[2] == 0 || p[2] == '/')) {
            return EACCES;
        }
    }

    if (!std::strncmp(buffer, "/app0", 5) || !std::strncmp(buffer, "/hostapp", 8)) {
        if (bb_vfs_is_layer_active(VFS_LAYER_BASE)) {
            const char *subpath = buffer + (buffer[1] == 'a' ? 5 : 8);
            bb_vfs_resolve_path(subpath, out, size);
            static int trace = -1;
            if (trace < 0) {
                const char *t = std::getenv("BB_FILE_TRACE");
                trace = (t && t[0] == '1') ? 1 : 0;
            }
            if (trace) {
                std::printf("File trace (vfs): %s -> %s\n", guest, out);
            }
            return 0;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(s_lock);
    size_t best = 0;
    const Mount *m = nullptr;
    for (size_t i = 0; i < s_mount_count; ++i) {
        const size_t n = std::strlen(s_mounts[i].guest);
        if (!std::strncmp(buffer, s_mounts[i].guest, n) && (buffer[n] == '/' || !buffer[n]) && n > best) {
            best = n;
            m = &s_mounts[i];
        }
    }
    int result = 0;
    if (!m) {
        result = ENOENT;
    } else if (static_cast<size_t>(std::snprintf(out, size, "%s%s", m->host, buffer + best)) >= size) {
        result = ENAMETOOLONG;
    }

    if (result == ENOENT) {
        std::fprintf(stderr, "Runtime: no mount for guest path %s\n", guest);
    }
    static int trace = -1;
    if (trace < 0) {
        const char *t = std::getenv("BB_FILE_TRACE");
        trace = (t && t[0] == '1') ? 1 : 0;
    }
    if (trace) {
        std::printf("File trace: %s -> %s\n", guest, result ? "(no mount)" : out);
    }
    return result;
}

int FixCase(char *path) {
    if (path[0] != '/') return 0;
    char out[1024] = "";
    char trial[1024];
    char part[256];
    const char *p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *end = std::strchr(p, '/');
        const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len >= sizeof(part)) return 0;
        std::memcpy(part, p, len);
        part[len] = 0;
        if (static_cast<size_t>(std::snprintf(trial, sizeof(trial), "%s/%s", out, part)) >= sizeof(trial)) {
            return 0;
        }
        if (access(trial, F_OK) != 0) {
            DIR *d = opendir(out[0] ? out : "/");
            if (!d) return 0;
            int found = 0;
            for (struct dirent *e = nullptr; (e = readdir(d));) {
                if (!strcasecmp(e->d_name, part)) {
                    std::snprintf(trial, sizeof(trial), "%s/%s", out, e->d_name);
                    found = 1;
                    break;
                }
            }
            closedir(d);
            if (!found) return 0;
        }
        std::memcpy(out, trial, std::strlen(trial) + 1);
        p += len;
    }
    if (!std::strcmp(out, path) || std::strlen(out) != std::strlen(path)) {
        return 0;
    }
    std::memcpy(path, out, std::strlen(out) + 1);
    return 1;
}

int HostFlags(int flags) {
    int r = 0;
    switch (flags & 3) {
    case 0:
        r = O_RDONLY;
        break;
    case 1:
        r = O_WRONLY;
        break;
    default:
        r = O_RDWR;
        break;
    }
    if (flags & 0x4) r |= O_NONBLOCK;
    if (flags & 0x8) r |= O_APPEND;
    if (flags & 0x80) r |= O_SYNC;
    if (flags & 0x200) r |= O_CREAT;
    if (flags & 0x400) r |= O_TRUNC;
    if (flags & 0x800) r |= O_EXCL;
    if (flags & 0x20000) r |= O_DIRECTORY;
    return r | O_CLOEXEC;
}

void FreeListing(Listing *l) {
    if (l) {
        std::free(l->names);
        std::free(l->offsets);
        std::free(l->types);
        std::free(l);
    }
}

Listing *ListDirectory(const char *path) {
    DIR *d = opendir(path);
    if (!d) return nullptr;
    auto *l = static_cast<Listing *>(std::calloc(1, sizeof(Listing)));
    size_t capacity = 0, bytes = 0, cap_names = 0;
    struct dirent *e = nullptr;
    while (l && (e = readdir(d))) {
        if (IsTempName(e->d_name)) continue;
        const size_t n = std::strlen(e->d_name) + 1;
        if (l->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            l->offsets = static_cast<size_t *>(std::realloc(l->offsets, capacity * sizeof(size_t)));
            l->types = static_cast<unsigned char *>(std::realloc(l->types, capacity));
        }
        if (bytes + n > cap_names) {
            cap_names = (bytes + n) * 2;
            l->names = static_cast<char *>(std::realloc(l->names, cap_names));
        }
        if (!l->offsets || !l->types || !l->names) {
            std::fputs("Out of memory listing directory\n", stderr);
            std::exit(1);
        }
        std::memcpy(l->names + bytes, e->d_name, n);
        l->offsets[l->count] = bytes;
        unsigned char type = e->d_type;
        if (type == DT_LNK || type == DT_UNKNOWN) {
            struct stat entry {};
            if (!fstatat(dirfd(d), e->d_name, &entry, 0)) {
                type = S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
            }
        }
        l->types[l->count] = (type == DT_DIR) ? 4 : (type == DT_REG) ? 8 : (type == DT_LNK) ? 10 : 0;
        ++l->count;
        bytes += n;
    }
    closedir(d);
    return l;
}

void ConvertStat(const struct stat *s, GuestStat *g) {
    std::memset(g, 0, sizeof(*g));
    g->dev = static_cast<uint32_t>(s->st_dev);
    g->ino = static_cast<uint32_t>(s->st_ino);
    g->mode = static_cast<uint16_t>(s->st_mode);
    g->nlink = static_cast<uint16_t>(s->st_nlink);
    g->size = s->st_size;
    g->blocks = s->st_blocks;
    g->blksize = static_cast<uint32_t>(s->st_blksize);
    g->atime = GuestTimespec{s->st_atim.tv_sec, s->st_atim.tv_nsec};
    g->mtime = GuestTimespec{s->st_mtim.tv_sec, s->st_mtim.tv_nsec};
    g->ctime = GuestTimespec{s->st_ctim.tv_sec, s->st_ctim.tv_nsec};
    g->birthtime = g->ctime;
}

File *GetFile(int fd) {
    if (fd < 3 || fd >= MAX_FILES || !s_files[fd].used) return nullptr;
    return &s_files[fd];
}

bool AudioTrace() {
    static int v = -1;
    if (v < 0) {
        const char *e = std::getenv("BB_AUDIO_TRACE");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v != 0;
}

bool SaveTrace() {
    static int v = -1;
    if (v < 0) {
        const char *e = std::getenv("BB_SAVE_TRACE");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v != 0;
}

bool IsGamePath(const char *p) {
    if (!p || !*p) return false;
    if (*p != '/') return true;
    return (!std::strncmp(p, "/app0", 5) && (!p[5] || p[5] == '/')) ||
           (!std::strncmp(p, "/hostapp", 8) && (!p[8] || p[8] == '/'));
}

int CopyContents(int from, int to) {
    char buffer[65536];
    for (;;) {
        const ssize_t n = read(from, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -1 : 0;
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(to, buffer + done, static_cast<size_t>(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) return -1;
            done += w;
        }
    }
}

void CurrentCopy(char *path, size_t size, int write_op) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    for (int i = 3; i < MAX_FILES; ++i) {
        if (s_files[i].used && s_files[i].commit && !std::strcmp(s_files[i].commit, path)) {
            std::snprintf(path, size, "%s", s_files[i].temp);
            s_files[i].dirty |= write_op;
            break;
        }
    }
}

int OpenForCommit(const char *path, const char *source, int flags, int mode, char **commit, char **temp) {
    const int hf = HostFlags(flags);
    struct stat s {};
    const int exists = !stat(source, &s);
    if (exists && !S_ISREG(s.st_mode)) {
        const int h = open(path, hf, mode ? mode : 0644);
        return h < 0 ? -errno : h;
    }
    if (exists && (hf & O_CREAT) && (hf & O_EXCL)) return -EEXIST;
    if (!exists && !(hf & O_CREAT)) return -ENOENT;
    const size_t n = std::strlen(path) + 32;
    char *t = static_cast<char *>(std::malloc(n));
    char *c = strdup(path);
    if (!t || !c) {
        std::free(t);
        std::free(c);
        return -ENOMEM;
    }
    std::snprintf(t, n, "%s.%u.bbtmp", path, s_temp_serial.fetch_add(1, std::memory_order_relaxed));
    const int out =
        open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, exists ? static_cast<int>(s.st_mode & 07777) : mode ? mode : 0644);
    int e = (out < 0) ? errno : 0;
    if (!e && exists && !(hf & O_TRUNC)) {
        const int in = open(source, O_RDONLY | O_CLOEXEC);
        if (in < 0 || CopyContents(in, out) != 0) {
            e = errno ? errno : EIO;
        }
        if (in >= 0) {
            close(in);
        }
    }
    if (out >= 0) {
        close(out);
    }
    const int host = e ? -1 : open(t, hf & ~(O_CREAT | O_EXCL | O_TRUNC));
    if (host < 0) {
        if (!e) e = errno;
        unlink(t);
        std::free(t);
        std::free(c);
        return -e;
    }
    *commit = c;
    *temp = t;
    return host;
}

void SyncDirectory(const char *file) {
    char dir[1024];
    std::snprintf(dir, sizeof(dir), "%s", file);
    char *slash = std::strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    const int d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) {
        fsync(d);
        close(d);
    }
}

int CommitFile(const File *f) {
    if (!f->dirty) {
        close(f->host);
        unlink(f->temp);
        return 0;
    }
    int e = fsync(f->host) ? errno : 0;
    close(f->host);
    if (!e && rename(f->temp, f->commit) != 0) {
        e = errno;
    }
    if (e) {
        std::fprintf(stderr, "Runtime: save file %s not replaced (%s); the old one is kept\n", f->path,
                     std::strerror(e));
        unlink(f->temp);
        return -e;
    }
    SyncDirectory(f->commit);
    return 0;
}

/* Flash/GFX options layout patching */
struct Bits {
    const uint8_t *p;
    size_t bit;
};

unsigned BitsRead(Bits *b, int n) {
    unsigned v = 0;
    while (n--) {
        v = (v << 1) | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        ++b->bit;
    }
    return v;
}

int SwfTag(const uint8_t *buf, size_t at, size_t end, unsigned *code, size_t *body, size_t *len, size_t *next) {
    if (at + 2 > end) return 0;
    const unsigned v = buf[at] | (buf[at + 1] << 8);
    *code = v >> 6;
    *len = v & 0x3f;
    *body = at + 2;
    if (*len == 0x3f) {
        if (at + 6 > end) return 0;
        *len = buf[at + 2] | (buf[at + 3] << 8) | (buf[at + 4] << 16) | (static_cast<size_t>(buf[at + 5]) << 24);
        *body = at + 6;
    }
    *next = *body + *len;
    return *next <= end;
}

uint8_t *SwfDepth(uint8_t *buf, unsigned code, size_t body, size_t len) {
    if ((code == 26 || code == 28) && len >= (code == 26 ? 3u : 2u)) {
        return buf + body + (code == 26 ? 1 : 0);
    }
    if (code == 70 && len >= 4) return buf + body + 2;
    if (code == 5 && len >= 4) return buf + body + 2;
    return nullptr;
}

const char *SwfPlaceName(const uint8_t *t, unsigned code, size_t len) {
    size_t p = (code == 70) ? 2 : 1;
    const unsigned flags = t[0];
    const unsigned flags2 = (code == 70) ? t[1] : 0;
    p += 2; /* depth */
    if (code == 70 && (flags2 & 0x08)) {
        while (p < len && t[p]) ++p;
        ++p;
    }
    if (flags & 0x02) p += 2; /* character id */
    if (!(flags & 0x20) || p >= len) return nullptr;
    Bits b = {t, p * 8};
    if (flags & 0x04) {
        if (BitsRead(&b, 1)) {
            const int n = BitsRead(&b, 5);
            BitsRead(&b, n);
            BitsRead(&b, n);
        }
        if (BitsRead(&b, 1)) {
            const int n = BitsRead(&b, 5);
            BitsRead(&b, n);
            BitsRead(&b, n);
        }
        const int n = BitsRead(&b, 5);
        BitsRead(&b, n);
        BitsRead(&b, n);
        p = (b.bit + 7) / 8;
    }
    if (flags & 0x08) {
        b.bit = p * 8;
        const int add = BitsRead(&b, 1);
        const int mult = BitsRead(&b, 1);
        const int n = BitsRead(&b, 4);
        if (mult) {
            for (int i = 0; i < 4; ++i) BitsRead(&b, n);
        }
        if (add) {
            for (int i = 0; i < 4; ++i) BitsRead(&b, n);
        }
        p = (b.bit + 7) / 8;
    }
    if (flags & 0x10) p += 2; /* ratio */
    if (p >= len || !std::memchr(t + p, 0, len - p)) return nullptr;
    return reinterpret_cast<const char *>(t + p);
}

int FixOptionLayout(uint8_t *buf, size_t size) {
    if (size < 16 || std::memcmp(buf, "GFX", 3) != 0) return 0;
    const size_t rect = (5 + 4 * (buf[8] >> 3) + 7) / 8;
    const size_t first = 8 + rect + 4;
    static const char wanted[] = "OptionSetting_fla._Option_ItemList_71";
    unsigned code = 0;
    size_t body = 0, len = 0, next = 0;
    long sprite = -1;
    for (size_t at = first; SwfTag(buf, at, size, &code, &body, &len, &next) && code; at = next) {
        if (code != 76 || len < 2) continue;
        size_t p = body + 2;
        for (unsigned i = 0, n = buf[body] | (buf[body + 1] << 8); i < n && p + 2 < next; ++i) {
            const unsigned id = buf[p] | (buf[p + 1] << 8);
            const auto *name = reinterpret_cast<const char *>(buf + p + 2);
            if (!std::memchr(name, 0, next - p - 2)) return 0;
            if (!std::strcmp(name, wanted)) sprite = id;
            p += 2 + std::strlen(name) + 1;
        }
    }
    if (sprite < 0) return 0;
    for (size_t at = first; SwfTag(buf, at, size, &code, &body, &len, &next) && code; at = next) {
        if (code != 39 || len < 4 || (buf[body] | (buf[body + 1] << 8)) != static_cast<unsigned>(sprite)) continue;
        unsigned depth[7] = {0};
        size_t inner = 0, ilen = 0, inext = 0;
        unsigned c = 0;
        for (size_t t = body + 4; SwfTag(buf, t, next, &c, &inner, &ilen, &inext) && c && c != 1; t = inext) {
            if (c != 26 && c != 70) continue;
            const char *name = SwfPlaceName(buf + inner, c, ilen);
            const uint8_t *d = SwfDepth(buf, c, inner, ilen);
            if (name && d && !std::strncmp(name, "Item_", 5) && name[5] >= '0' && name[5] <= '6' &&
                !std::strcmp(name + 6, "_0")) {
                depth[name[5] - '0'] = d[0] | (d[1] << 8);
            }
        }
        for (int i = 0; i < 7; ++i) {
            if (!depth[i] || (i && depth[i] <= depth[i - 1])) return 0;
        }
        for (size_t t = body + 4; SwfTag(buf, t, next, &c, &inner, &ilen, &inext) && c; t = inext) {
            uint8_t *d = SwfDepth(buf, c, inner, ilen);
            if (!d) continue;
            const unsigned v = d[0] | (d[1] << 8);
            for (int i = 0; i < 7; ++i) {
                if (v == depth[i]) {
                    d[0] = depth[6 - i] & 0xff;
                    d[1] = static_cast<uint8_t>(depth[6 - i] >> 8);
                    break;
                }
            }
        }
        return 1;
    }
    return 0;
}

int MenuLayoutFile(const char *guest, int host) {
    static const char suffix[] = "/menu/optionsetting.gfx";
    const char *env = std::getenv("BB_GAME_MENU");
    const size_t n = std::strlen(guest);
    const size_t k = sizeof(suffix) - 1;
    if ((env && env[0] == '0') || n < k || strcasecmp(guest + n - k, suffix) != 0) {
        return host;
    }
    struct stat s {};
    if (fstat(host, &s) != 0 || s.st_size <= 0 || s.st_size > (16 << 20)) {
        return host;
    }
    auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(s.st_size)));
    if (!buf) return host;
    if (pread(host, buf, static_cast<size_t>(s.st_size), 0) != s.st_size ||
        !FixOptionLayout(buf, static_cast<size_t>(s.st_size))) {
        std::free(buf);
        return host;
    }
    const int fixed = memfd_create("optionsetting.gfx", MFD_CLOEXEC);
    if (fixed < 0 || write(fixed, buf, static_cast<size_t>(s.st_size)) != s.st_size || lseek(fixed, 0, SEEK_SET) != 0) {
        if (fixed >= 0) close(fixed);
        std::free(buf);
        return host;
    }
    std::free(buf);
    close(host);
    static int told = 0;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED)) {
        std::printf("Runtime: %s: the settings rows stack downwards (pop-up lists cover the rows below)\n", guest);
    }
    return fixed;
}

int HostFd(int fd) {
    if (fd >= 0 && fd < 3) return fd;
    File *f = GetFile(fd);
    return f ? f->host : -1;
}

int HostFdWritten(int fd) {
    if (fd >= 0 && fd < 3) return fd;
    File *f = GetFile(fd);
    if (!f) return -1;
    if (!f->dirty && SaveTrace() && IsSavePath(f->path)) {
        std::printf("Save trace: first write to fd %d (%s)\n", fd, f->path);
    }
    f->dirty = 1;
    return f->host;
}

void TouchForWrite(void *buffer, uint64_t size) {
    if (!size) return;
    auto p = reinterpret_cast<uintptr_t>(buffer) & ~static_cast<uintptr_t>(4095);
    const auto end = reinterpret_cast<uintptr_t>(buffer) + size;
    for (; p < end; p += 4096) {
        auto *b = reinterpret_cast<volatile unsigned char *>(
            p < reinterpret_cast<uintptr_t>(buffer) ? reinterpret_cast<uintptr_t>(buffer) : p);
        *b = *b;
    }
}

ssize_t ReadRetrying(int h, void *buffer, uint64_t size, int64_t offset, int positional) {
    ssize_t n = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        n = positional ? pread(h, buffer, size, offset) : read(h, buffer, size);
        if (n >= 0 || errno != EFAULT) break;
        TouchForWrite(buffer, size);
    }
    return n;
}

void TouchForRead(const void *buffer, uint64_t size) {
    if (!size) return;
    auto p = reinterpret_cast<uintptr_t>(buffer) & ~static_cast<uintptr_t>(4095);
    const auto end = reinterpret_cast<uintptr_t>(buffer) + size;
    for (; p < end; p += 4096) {
        (void)*reinterpret_cast<volatile const unsigned char *>(
            p < reinterpret_cast<uintptr_t>(buffer) ? reinterpret_cast<uintptr_t>(buffer) : p);
    }
}

int64_t do_open(const char *guest, int flags, int mode) {
    if (IsGamePath(guest) && (flags & (3 | 0x8 | 0x200 | 0x400 | 0x800))) {
        return -EROFS;
    }
    char path[1024];
    int e = Translate(guest, path, sizeof(path));
    if (e) return -e;
    char *commit = nullptr, *temp = nullptr;
    char current[1024];
    std::snprintf(current, sizeof(current), "%s", path);
    if (IsSavePath(guest)) {
        CurrentCopy(current, sizeof(current), 0);
    }
    int host = -1;
    if (IsSavePath(guest) && (flags & 3)) {
        host = OpenForCommit(path, current, flags, mode, &commit, &temp);
        if (host < 0) {
            errno = -host;
            host = -1;
        }
    } else {
        host = open(current, HostFlags(flags), mode ? mode : 0644);
        if (host < 0 && errno == ENOENT && !(flags & 0x200) && !IsSavePath(guest) && FixCase(current)) {
            host = open(current, HostFlags(flags), mode ? mode : 0644);
            std::snprintf(path, sizeof(path), "%s", current);
        }
    }
    if (host >= 0 && !(flags & 3) && IsGamePath(guest)) {
        host = MenuLayoutFile(guest, host);
    }
    if (SaveTrace() && IsSavePath(guest)) {
        std::printf("Save trace: open(%s, flags 0x%x) -> host %d%s%s\n", guest, flags, host,
                    temp ? ", copy " : "", temp ? temp : "");
    }
    if (host < 0) {
        e = errno;
        if (e == ENOENT) {
            s_missing.fetch_add(1, std::memory_order_relaxed);
            std::printf("Runtime: open(%s) -> not found\n", guest);
        }
        return -e;
    }
    struct stat s {};
    Listing *dir = nullptr;
    if (fstat(host, &s) == 0 && S_ISDIR(s.st_mode)) {
        dir = ListDirectory(path);
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    int fd = -1;
    for (int i = 3; i < MAX_FILES; ++i) {
        if (!s_files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        close(host);
        FreeListing(dir);
        if (temp) unlink(temp);
        std::free(commit);
        std::free(temp);
        return -EMFILE;
    }
    s_files[fd] = File{.used = 1, .host = host, .dirty = 0, .dir = dir, .position = 0, .path = {0}, .commit = commit, .temp = temp};
    std::snprintf(s_files[fd].path, sizeof(s_files[fd].path), "%s", guest);
    s_opens.fetch_add(1, std::memory_order_relaxed);

    if (AudioTrace() && std::strstr(guest, "sound/")) {
        std::printf("Audio trace: open(%s) -> fd %d, %lld bytes\n", guest, fd, static_cast<long long>(s.st_size));
    }
    const char *mod_trace = std::getenv("BB_MOD_TRACE");
    const char *mod_root = std::getenv("BB_MODS_DIR");
    if (mod_trace && mod_trace[0] == '1' && mod_root) {
        char actual[PATH_MAX], root[PATH_MAX];
        static unsigned traced = 0;
        if (realpath(path, actual) && realpath(mod_root, root)) {
            const size_t n = std::strlen(root);
            if (!std::strncmp(actual, root, n) && actual[n] == '/' &&
                __atomic_fetch_add(&traced, 1, __ATOMIC_RELAXED) < 32) {
                std::printf("Mods: open %s -> %s\n", guest, actual);
            }
        }
    }
    if (SaveTrace() && IsSavePath(guest)) {
        std::printf("Save trace: open(%s) -> fd %d\n", guest, fd);
    }
    return fd;
}

int64_t do_close(int fd) {
    if (fd >= 0 && fd < 3) return 0;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    File *f = GetFile(fd);
    if (!f) return -EBADF;
    File closed = *f;
    *f = File{};
    FreeListing(closed.dir);
    int result = 0;
    if (closed.temp) {
        result = CommitFile(&closed);
    } else {
        close(closed.host);
    }
    if (SaveTrace() && IsSavePath(closed.path)) {
        std::printf("Save trace: close(fd %d, %s)%s -> %d\n", fd, closed.path,
                    closed.temp ? (closed.dirty ? " commit" : " unwritten") : "", result);
    }
    std::free(closed.commit);
    std::free(closed.temp);
    return result;
}

int64_t do_read(int fd, void *buffer, uint64_t size) {
    const int h = HostFd(fd);
    if (h < 0) return -EBADF;
    runtime_memory_note_write(reinterpret_cast<uintptr_t>(buffer), size);
    TouchForWrite(buffer, size);
    const ssize_t n = ReadRetrying(h, buffer, size, 0, 0);
    if (n < 0) {
        if (AudioTrace()) {
            std::printf("Audio trace: read(fd %d, %llu) failed, errno %d\n", fd,
                        static_cast<unsigned long long>(size), errno);
        }
        return -errno;
    }
    if (n > 0) {
        runtime_memory_note_write(reinterpret_cast<uintptr_t>(buffer), static_cast<uint64_t>(n));
    }
    s_reads.fetch_add(1, std::memory_order_relaxed);
    s_bytes_read.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return n;
}

int64_t do_pread(int fd, void *buffer, uint64_t size, int64_t offset) {
    const int h = HostFd(fd);
    if (h < 0) return -EBADF;
    runtime_memory_note_write(reinterpret_cast<uintptr_t>(buffer), size);
    TouchForWrite(buffer, size);
    const ssize_t n = ReadRetrying(h, buffer, size, offset, 1);
    if (n < 0) {
        if (AudioTrace()) {
            std::printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n", fd,
                        static_cast<unsigned long long>(size), static_cast<long long>(offset), errno);
        }
        return -errno;
    }
    if (n > 0) {
        runtime_memory_note_write(reinterpret_cast<uintptr_t>(buffer), static_cast<uint64_t>(n));
    }
    s_reads.fetch_add(1, std::memory_order_relaxed);
    s_bytes_read.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return n;
}

int64_t do_write(int fd, const void *buffer, uint64_t size) {
    const int h = HostFdWritten(fd);
    if (h < 0) return -EBADF;
    TouchForRead(buffer, size);
    const ssize_t n = write(h, buffer, size);
    if (n < 0) return -errno;
    s_writes.fetch_add(1, std::memory_order_relaxed);
    return n;
}

int64_t do_pwrite(int fd, const void *buffer, uint64_t size, int64_t offset) {
    const int h = HostFdWritten(fd);
    if (h < 0) return -EBADF;
    TouchForRead(buffer, size);
    const ssize_t n = pwrite(h, buffer, size, offset);
    return (n < 0) ? -errno : n;
}

int64_t do_lseek(int fd, int64_t offset, int whence) {
    if (SaveTrace()) {
        File *t = GetFile(fd);
        if (t && IsSavePath(t->path)) {
            std::printf("Save trace: lseek(fd %d, %lld, %d)\n", fd, static_cast<long long>(offset), whence);
        }
    }
    File *f = GetFile(fd);
    if (!f) return -EBADF;
    if (whence < 0 || whence > 2) return -EINVAL;
    if (f->dir) {
        if (whence == 0 && offset >= 0) {
            f->position = static_cast<size_t>(offset);
            return offset;
        }
        return -EINVAL;
    }
    const off_t r = lseek(f->host, offset, whence);
    return (r < 0) ? -errno : r;
}

int64_t do_fstat(int fd, GuestStat *out) {
    if (SaveTrace()) {
        File *t = GetFile(fd);
        if (t && IsSavePath(t->path)) {
            std::printf("Save trace: fstat(fd %d)\n", fd);
        }
    }
    const int h = HostFd(fd);
    if (h < 0) return -EBADF;
    if (!out) return -EFAULT;
    struct stat s {};
    if (fstat(h, &s) != 0) return -errno;
    ConvertStat(&s, out);
    return 0;
}

int64_t do_stat(const char *guest, GuestStat *out) {
    char path[1024];
    struct stat s {};
    const int e = Translate(guest, path, sizeof(path));
    if (e) return -e;
    if (IsSavePath(guest)) {
        CurrentCopy(path, sizeof(path), 0);
    }
    if (!out) return -EFAULT;
    int r = stat(path, &s) ? -errno : 0;
    if (r == -ENOENT && !IsSavePath(guest) && FixCase(path)) {
        r = stat(path, &s) ? -errno : 0;
    }
    if (SaveTrace() && IsSavePath(guest)) {
        std::printf("Save trace: stat(%s) -> %d, %lld bytes\n", guest, r,
                    r ? -1LL : static_cast<long long>(s.st_size));
    }
    if (r) return r;
    ConvertStat(&s, out);
    return 0;
}

int64_t do_getdents(int fd, char *buffer, uint64_t size, int64_t *basep) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    File *f = GetFile(fd);
    int64_t result = 0;
    if (!f) {
        result = -EBADF;
    } else if (!f->dir) {
        result = -EINVAL;
    } else if (!buffer) {
        result = -EFAULT;
    } else if (size < 512) {
        result = -EINVAL;
    } else {
        if (basep) *basep = static_cast<int64_t>(f->position);
        uint64_t written = 0;
        while (f->position < f->dir->count) {
            const char *name = f->dir->names + f->dir->offsets[f->position];
            size_t n = std::strlen(name);
            if (n > 255) n = 255;
            const uint16_t reclen = static_cast<uint16_t>((8 + n + 1 + 7) & ~static_cast<size_t>(7));
            if (written + reclen > size) break;
            char *p = buffer + written;
            std::memset(p, 0, reclen);
            const uint32_t ino = static_cast<uint32_t>(f->position + 1);
            std::memcpy(p, &ino, 4);
            std::memcpy(p + 4, &reclen, 2);
            p[6] = static_cast<char>(f->dir->types[f->position]);
            p[7] = static_cast<char>(n);
            std::memcpy(p + 8, name, n);
            written += reclen;
            ++f->position;
        }
        result = static_cast<int64_t>(written);
    }
    return result;
}

int64_t path_op(const char *guest, int op, int mode) {
    if (IsGamePath(guest)) return -EROFS;
    char path[1024];
    const int e = Translate(guest, path, sizeof(path));
    if (e) return -e;
    int r = (op == 0) ? mkdir(path, mode ? mode : 0755) : (op == 1) ? rmdir(path) : unlink(path);
    r = r ? -errno : 0;
    if (SaveTrace() && IsSavePath(guest)) {
        std::printf("Save trace: %s(%s) -> %d\n", (op == 0) ? "mkdir" : (op == 1) ? "rmdir" : "unlink", guest, r);
    }
    return r;
}

int64_t do_rename(const char *from, const char *to) {
    if (IsGamePath(from) || IsGamePath(to)) return -EROFS;
    char a[1024], b[1024];
    int e = Translate(from, a, sizeof(a));
    if (!e) e = Translate(to, b, sizeof(b));
    if (e) return -e;
    const int r = rename(a, b) ? -errno : 0;
    if (SaveTrace() && (IsSavePath(from) || IsSavePath(to))) {
        std::printf("Save trace: rename(%s, %s) -> %d\n", from, to, r);
    }
    return r;
}

int64_t do_ftruncate(int fd, int64_t length) {
    const int h = HostFdWritten(fd);
    if (h < 0) return -EBADF;
    return ftruncate(h, length) ? -errno : 0;
}

int64_t do_truncate(const char *guest, int64_t length) {
    if (IsGamePath(guest)) return -EROFS;
    char path[1024];
    const int e = Translate(guest, path, sizeof(path));
    if (e) return -e;
    if (IsSavePath(guest)) {
        CurrentCopy(path, sizeof(path), 1);
    }
    if (SaveTrace() && IsSavePath(guest)) {
        std::printf("Save trace: truncate(%s, %lld)\n", guest, static_cast<long long>(length));
    }
    return truncate(path, length) ? -errno : 0;
}

int64_t do_fsync(int fd) {
    const int h = HostFd(fd);
    if (h < 0) return -EBADF;
    return fsync(h) ? -errno : 0;
}

int64_t do_access(const char *guest, int mode) {
    char path[1024];
    const int e = Translate(guest, path, sizeof(path));
    if (e) return -e;
    if (IsSavePath(guest)) {
        CurrentCopy(path, sizeof(path), 0);
    }
    int r = access(path, mode & 7) ? -errno : 0;
    if (r == -ENOENT && !IsSavePath(guest) && FixCase(path)) {
        r = access(path, mode & 7) ? -errno : 0;
    }
    return r;
}

int64_t SceErr(int64_t r) {
    return r < 0 ? ERR(runtime_guest_errno(static_cast<int>(-r))) : r;
}

int64_t PosixErr(int64_t r) {
    if (r < 0) {
        *runtime_errno() = runtime_guest_errno(static_cast<int>(-r));
        return -1;
    }
    return r;
}

#define FILE_PAIR(name, params, args)                         \
    ABI int64_t sce_##name params { return SceErr(do_##name args); } \
    ABI int64_t posix_##name params { return PosixErr(do_##name args); }

FILE_PAIR(open, (const char *p, int f, int m), (p, f, m))
FILE_PAIR(close, (int fd), (fd))
FILE_PAIR(read, (int fd, void *b, uint64_t n), (fd, b, n))
FILE_PAIR(pread, (int fd, void *b, uint64_t n, int64_t o), (fd, b, n, o))
FILE_PAIR(write, (int fd, const void *b, uint64_t n), (fd, b, n))
FILE_PAIR(pwrite, (int fd, const void *b, uint64_t n, int64_t o), (fd, b, n, o))
FILE_PAIR(lseek, (int fd, int64_t o, int w), (fd, o, w))
FILE_PAIR(fstat, (int fd, GuestStat *s), (fd, s))
FILE_PAIR(stat, (const char *p, GuestStat *s), (p, s))
FILE_PAIR(getdents, (int fd, char *b, uint64_t n, int64_t *base), (fd, b, n, base))
FILE_PAIR(rename, (const char *a, const char *b), (a, b))
FILE_PAIR(ftruncate, (int fd, int64_t l), (fd, l))
FILE_PAIR(truncate, (const char *p, int64_t l), (p, l))
FILE_PAIR(fsync, (int fd), (fd))

ABI int64_t posix_access(const char *p, int m) {
    return PosixErr(do_access(p, m));
}
ABI int64_t sce_mkdir(const char *p, int m) {
    return SceErr(path_op(p, 0, m));
}
ABI int64_t posix_mkdir(const char *p, int m) {
    return PosixErr(path_op(p, 0, m));
}
ABI int64_t sce_rmdir(const char *p) {
    return SceErr(path_op(p, 1, 0));
}
ABI int64_t posix_rmdir(const char *p) {
    return PosixErr(path_op(p, 1, 0));
}
ABI int64_t sce_unlink(const char *p) {
    return SceErr(path_op(p, 2, 0));
}
ABI int64_t posix_unlink(const char *p) {
    return PosixErr(path_op(p, 2, 0));
}
ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s{};
    return static_cast<int32_t>(SceErr(do_stat(p, &s)));
}

const RuntimeExport file_exports[] = {
    {"sceKernelOpen", reinterpret_cast<void *>(sce_open)},
    {"open", reinterpret_cast<void *>(posix_open)},
    {"_open", reinterpret_cast<void *>(posix_open)},
    {"sceKernelClose", reinterpret_cast<void *>(sce_close)},
    {"close", reinterpret_cast<void *>(posix_close)},
    {"_close", reinterpret_cast<void *>(posix_close)},
    {"sceKernelRead", reinterpret_cast<void *>(sce_read)},
    {"read", reinterpret_cast<void *>(posix_read)},
    {"_read", reinterpret_cast<void *>(posix_read)},
    {"sceKernelPread", reinterpret_cast<void *>(sce_pread)},
    {"pread", reinterpret_cast<void *>(posix_pread)},
    {"sceKernelWrite", reinterpret_cast<void *>(sce_write)},
    {"write", reinterpret_cast<void *>(posix_write)},
    {"_write", reinterpret_cast<void *>(posix_write)},
    {"sceKernelPwrite", reinterpret_cast<void *>(sce_pwrite)},
    {"pwrite", reinterpret_cast<void *>(posix_pwrite)},
    {"sceKernelLseek", reinterpret_cast<void *>(sce_lseek)},
    {"lseek", reinterpret_cast<void *>(posix_lseek)},
    {"sceKernelFstat", reinterpret_cast<void *>(sce_fstat)},
    {"fstat", reinterpret_cast<void *>(posix_fstat)},
    {"sceKernelStat", reinterpret_cast<void *>(sce_stat)},
    {"stat", reinterpret_cast<void *>(posix_stat)},
    {"sceKernelGetdirentries", reinterpret_cast<void *>(sce_getdents)},
    {"getdirentries", reinterpret_cast<void *>(posix_getdents)},
    {"sceKernelRename", reinterpret_cast<void *>(sce_rename)},
    {"rename", reinterpret_cast<void *>(posix_rename)},
    {"sceKernelFtruncate", reinterpret_cast<void *>(sce_ftruncate)},
    {"ftruncate", reinterpret_cast<void *>(posix_ftruncate)},
    {"sceKernelTruncate", reinterpret_cast<void *>(sce_truncate)},
    {"truncate", reinterpret_cast<void *>(posix_truncate)},
    {"sceKernelFsync", reinterpret_cast<void *>(sce_fsync)},
    {"fsync", reinterpret_cast<void *>(posix_fsync)},
    {"access", reinterpret_cast<void *>(posix_access)},
    {"sceKernelCheckReachability", reinterpret_cast<void *>(sce_check_reachability)},
    {"sceKernelMkdir", reinterpret_cast<void *>(sce_mkdir)},
    {"mkdir", reinterpret_cast<void *>(posix_mkdir)},
    {"sceKernelRmdir", reinterpret_cast<void *>(sce_rmdir)},
    {"rmdir", reinterpret_cast<void *>(posix_rmdir)},
    {"sceKernelUnlink", reinterpret_cast<void *>(sce_unlink)},
    {"unlink", reinterpret_cast<void *>(posix_unlink)},
};

} // namespace runtime::file

extern "C" {

int runtime_file_mount(const char *guest, const char *host) {
    return runtime::file::MountGuest(guest, host);
}

void runtime_file_unmount(const char *guest) {
    runtime::file::UnmountGuest(guest);
}

const char *runtime_file_user_dir(void) {
    return runtime::file::GetUserDir();
}

void runtime_file_configure(const char *app0, const char *user) {
    runtime::file::Configure(app0, user);
}

int runtime_file_host_path(const char *guest, char *out, size_t size) {
    const int e = runtime::file::Translate(guest, out, size);
    if (e) return e;
    if (access(out, F_OK) != 0 && !runtime::file::FixCase(out)) return ENOENT;
    return 0;
}

void runtime_file_register(void) {
    runtime_register_exports(runtime::file::file_exports,
                             sizeof(runtime::file::file_exports) / sizeof(runtime::file::file_exports[0]));
}

uintptr_t runtime_file_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

int64_t runtime_file_open(const char *p, int f, int m) {
    return runtime::file::do_open(p, f, m);
}

int64_t runtime_file_close(int fd) {
    return runtime::file::do_close(fd);
}

int64_t runtime_file_read(int fd, void *b, uint64_t n) {
    return runtime::file::do_read(fd, b, n);
}

int64_t runtime_file_pread(int fd, void *b, uint64_t n, int64_t o) {
    return runtime::file::do_pread(fd, b, n, o);
}

int64_t runtime_file_write(int fd, const void *b, uint64_t n) {
    return runtime::file::do_write(fd, b, n);
}

int64_t runtime_file_lseek(int fd, int64_t o, int w) {
    return runtime::file::do_lseek(fd, o, w);
}

int64_t runtime_file_stat(const char *p, void *s) {
    return runtime::file::do_stat(p, static_cast<runtime::file::GuestStat *>(s));
}

int64_t runtime_file_fstat(int fd, void *s) {
    return runtime::file::do_fstat(fd, static_cast<runtime::file::GuestStat *>(s));
}

int64_t runtime_file_getdents(int fd, char *b, uint64_t n, int64_t *base) {
    return runtime::file::do_getdents(fd, b, n, base);
}

int runtime_file_translate(const char *guest, char *out, size_t size) {
    return runtime::file::Translate(guest, out, size);
}

void runtime_file_report(void) {
    std::printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
                runtime::file::s_opens.load(std::memory_order_relaxed),
                runtime::file::s_reads.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(runtime::file::s_bytes_read.load(std::memory_order_relaxed)),
                runtime::file::s_writes.load(std::memory_order_relaxed),
                runtime::file::s_missing.load(std::memory_order_relaxed));
}

} // extern "C"

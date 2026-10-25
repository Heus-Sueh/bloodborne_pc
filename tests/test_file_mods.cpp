/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "runtime.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int32_t s_guest_errno = 0;
extern "C" {
int32_t *runtime_errno(void) {
    return &s_guest_errno;
}
int32_t runtime_guest_errno(int e) {
    return e;
}
/* Reads into guest memory tell the GPU side (runtime_memory.c); nothing to tell here. */
void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    (void)address;
    (void)size;
}
void runtime_register_exports(const RuntimeExport *table, size_t count) {
    (void)table;
    (void)count;
}
uintptr_t runtime_registry_resolve(const char *name, int is_data) {
    (void)name;
    (void)is_data;
    return 0;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t n, const char *name) {
    for (size_t i = 0; i < n; ++i) {
        if (!std::strcmp(table[i].name, name)) {
            return reinterpret_cast<uintptr_t>(table[i].function);
        }
    }
    return 0;
}
}

#include "../src/runtime_file.cpp"

using namespace runtime::file;

int main(void) {
    char root[] = "/tmp/bbport-mod-files-XXXXXX";
    assert(mkdtemp(root));
    char game[512], user[512], source[512], link[512];
    std::snprintf(game, sizeof(game), "%s/game", root);
    std::snprintf(user, sizeof(user), "%s/user", root);
    std::snprintf(source, sizeof(source), "%s/mod.dcx", root);
    std::snprintf(link, sizeof(link), "%s/game/asset.dcx", root);
    assert(!mkdir(game, 0755));
    FILE *f = std::fopen(source, "w");
    assert(f);
    assert(std::fputs("modded", f) >= 0);
    assert(!std::fclose(f));
    assert(!symlink(source, link));
    runtime_file_configure(game, user);
    for (int i = 0; i < 3; ++i) {
        const char *path = i == 0 ? "/app0/asset.dcx" : i == 1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd = static_cast<int>(do_open(path, 0, 0));
        assert(fd >= 3);
        char content[8] = {0};
        assert(do_read(fd, content, 6) == 6 && !std::strcmp(content, "modded"));
        GuestStat info{};
        assert(!do_stat(path, &info) && info.size == 6);
        assert(!do_close(fd));
        assert(do_open(path, 2, 0) == -EROFS);
        assert(do_open(path, 0x400, 0) == -EROFS);
        assert(do_truncate(path, 0) == -EROFS);
        assert(path_op(path, 2, 0) == -EROFS);
        assert(do_rename(path, "/data/moved") == -EROFS);
    }
    int dir = static_cast<int>(do_open("/app0", 0x20000, 0));
    assert(dir >= 3);
    char entries[1024];
    int64_t count = do_getdents(dir, entries, sizeof(entries), nullptr);
    assert(count > 0);
    int found = 0;
    for (int64_t p = 0; p < count;) {
        uint16_t length = 0;
        std::memcpy(&length, entries + p + 4, 2);
        assert(length);
        if (!std::strcmp(entries + p + 8, "asset.dcx")) {
            assert(entries[p + 6] == 8);
            found = 1;
        }
        p += length;
    }
    assert(found && !do_close(dir));
    int save = static_cast<int>(do_open("/data/test-save", 0x202, 0644));
    assert(save >= 3);
    assert(do_write(save, "save", 4) == 4 && !do_close(save));
    assert(!path_op("/data/test-save", 2, 0));
    assert(!unlink(link) && !unlink(source) && !rmdir(game));
    const char *dirs[] = {"temp0", "download0", "data"};
    for (int i = 0; i < 3; ++i) {
        char p[1024];
        std::snprintf(p, sizeof(p), "%s/%s", user, dirs[i]);
        assert(!rmdir(p));
    }
    assert(!rmdir(user) && !rmdir(root));
    std::puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS");
    return 0;
}

#define _GNU_SOURCE
#include "runtime_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static BbVfsLayer g_layers[VFS_LAYER_COUNT] = {
    [VFS_LAYER_BASE]  = { .type = VFS_LAYER_BASE,  .name = "Base Game",     .mount_path = {0}, .active = false },
    [VFS_LAYER_PATCH] = { .type = VFS_LAYER_PATCH, .name = "Update Patch",  .mount_path = {0}, .active = false },
    [VFS_LAYER_DLC]   = { .type = VFS_LAYER_DLC,   .name = "DLC Add-ons",   .mount_path = {0}, .active = false },
    [VFS_LAYER_MOD]   = { .type = VFS_LAYER_MOD,   .name = "Mod Overlay",   .mount_path = {0}, .active = false },
    [VFS_LAYER_SAVE]  = { .type = VFS_LAYER_SAVE,  .name = "User SaveData", .mount_path = {0}, .active = false },
};

void bb_vfs_init(void) {
    for (int i = 0; i < VFS_LAYER_COUNT; ++i) {
        g_layers[i].active = false;
        memset(g_layers[i].mount_path, 0, sizeof(g_layers[i].mount_path));
    }
}

int bb_vfs_mount(BbVfsLayerType layer, const char *host_dir) {
    if ((int)layer < 0 || layer >= VFS_LAYER_COUNT || !host_dir) {
        return -1;
    }
    struct stat st;
    if (stat(host_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    strncpy(g_layers[layer].mount_path, host_dir, sizeof(g_layers[layer].mount_path) - 1);
    g_layers[layer].mount_path[sizeof(g_layers[layer].mount_path) - 1] = '\0';
    g_layers[layer].active = true;
    return 0;
}

void bb_vfs_unmount(BbVfsLayerType layer) {
    if ((int)layer >= 0 && layer < VFS_LAYER_COUNT) {
        g_layers[layer].active = false;
        memset(g_layers[layer].mount_path, 0, sizeof(g_layers[layer].mount_path));
    }
}

bool bb_vfs_is_layer_active(BbVfsLayerType layer) {
    if ((int)layer < 0 || layer >= VFS_LAYER_COUNT) {
        return false;
    }
    return g_layers[layer].active;
}

const char *bb_vfs_get_mount(BbVfsLayerType layer) {
    if ((int)layer < 0 || layer >= VFS_LAYER_COUNT || !g_layers[layer].active) {
        return NULL;
    }
    return g_layers[layer].mount_path;
}

static const char *normalize_guest_path(const char *path) {
    if (!path) return "";
    while (*path == '/') path++;
    if (strncmp(path, "app0/", 5) == 0) {
        path += 5;
    }
    return path;
}

bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len) {
    if (!guest_path || !host_path_out || max_len == 0) {
        return false;
    }
    const char *rel_path = normalize_guest_path(guest_path);

    /* Search layers in descending precedence order (highest to lowest) */
    for (int i = VFS_LAYER_COUNT - 1; i >= 0; --i) {
        if (!g_layers[i].active) continue;

        char candidate[2048];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", g_layers[i].mount_path, rel_path);
        if (written > 0 && (size_t)written < sizeof(candidate)) {
            if (access(candidate, F_OK) == 0) {
                strncpy(host_path_out, candidate, max_len - 1);
                host_path_out[max_len - 1] = '\0';
                return true;
            }
        }
    }

    /* Fallback candidate to base mount or first active layer */
    for (int i = 0; i < VFS_LAYER_COUNT; ++i) {
        if (g_layers[i].active) {
            snprintf(host_path_out, max_len, "%s/%s", g_layers[i].mount_path, rel_path);
            return false;
        }
    }

    host_path_out[0] = '\0';
    return false;
}

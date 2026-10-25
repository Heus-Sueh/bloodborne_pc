#ifndef BB_RUNTIME_VFS_H
#define BB_RUNTIME_VFS_H

#include "platform/platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Virtual File System layer types ordered by resolution precedence (lowest to highest) */
typedef enum {
    VFS_LAYER_BASE = 0,    /* Base game assets (/app0/dvdroot/ from PKG/ISO) */
    VFS_LAYER_PATCH,       /* Title update 1.09 patch overlay */
    VFS_LAYER_DLC,         /* Downloadable content add-ons (/addcont/...) */
    VFS_LAYER_MOD,         /* User modifications overlay */
    VFS_LAYER_SAVE,        /* Isolated user save data directory */
    VFS_LAYER_COUNT
} BbVfsLayerType;

/* Layer descriptor */
typedef struct {
    BbVfsLayerType type;
    const char *name;
    char mount_path[1024];
    bool active;
} BbVfsLayer;

/* Initializes the Virtual File System. */
void bb_vfs_init(void);

/* Mounts a host directory to a specific VFS layer. */
int bb_vfs_mount(BbVfsLayerType layer, const char *host_dir);

/* Unmounts a specific VFS layer. */
void bb_vfs_unmount(BbVfsLayerType layer);

/* Returns whether a specific layer is active. */
bool bb_vfs_is_layer_active(BbVfsLayerType layer);

/* Retrieves the mounted host path of a layer. Returns NULL if inactive. */
const char *bb_vfs_get_mount(BbVfsLayerType layer);

/* Resolves a guest path (e.g. "/app0/dvdroot/param.sfo") across the layered hierarchy.
 * Traverses layers in reverse precedence order (Save -> Mod -> DLC -> Patch -> Base).
 * Returns true if resolved to an existing host file, false otherwise. */
bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* BB_RUNTIME_VFS_H */

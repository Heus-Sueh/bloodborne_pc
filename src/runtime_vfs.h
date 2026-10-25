#ifndef BB_RUNTIME_VFS_H
#define BB_RUNTIME_VFS_H

#ifdef __cplusplus
#include "runtime_vfs.hpp"
#else

#include "platform/platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Virtual File System layer types ordered by resolution precedence */
typedef enum {
    VFS_LAYER_BASE = 0,    /* Base game assets (/app0/dvdroot/ from PKG/ISO) */
    VFS_LAYER_PATCH,       /* Title update 1.09 patch overlay */
    VFS_LAYER_DLC,         /* Downloadable content add-ons (/addcont/...) */
    VFS_LAYER_MOD,         /* User modifications overlay */
    VFS_LAYER_SAVE,        /* Isolated user save data directory */
    VFS_LAYER_COUNT
} BbVfsLayerType;

typedef struct {
    BbVfsLayerType type;
    const char *name;
    char mount_path[1024];
    bool active;
} BbVfsLayer;

void bb_vfs_init(void);
int bb_vfs_mount(BbVfsLayerType layer, const char *host_dir);
void bb_vfs_unmount(BbVfsLayerType layer);
bool bb_vfs_is_layer_active(BbVfsLayerType layer);
const char *bb_vfs_get_mount(BbVfsLayerType layer);
bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len);

#endif /* __cplusplus */

#endif /* BB_RUNTIME_VFS_H */

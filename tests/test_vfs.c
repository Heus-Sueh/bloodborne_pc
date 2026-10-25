#define _GNU_SOURCE
#include "runtime_vfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void) {
    printf("[VFS TEST] Initializing...\n");

    bb_vfs_init();

    /* 1. Initial layer queries */
    for (int i = 0; i < VFS_LAYER_COUNT; ++i) {
        assert(!bb_vfs_is_layer_active((BbVfsLayerType)i));
        assert(bb_vfs_get_mount((BbVfsLayerType)i) == NULL);
    }

    /* 2. Create temporary directories for testing layers */
    char base_dir[] = "/tmp/bb_vfs_base_XXXXXX";
    char mod_dir[] = "/tmp/bb_vfs_mod_XXXXXX";
    assert(mkdtemp(base_dir) != NULL);
    assert(mkdtemp(mod_dir) != NULL);

    /* 3. Mount layers */
    int m_base = bb_vfs_mount(VFS_LAYER_BASE, base_dir);
    int m_mod = bb_vfs_mount(VFS_LAYER_MOD, mod_dir);
    assert(m_base == 0);
    assert(m_mod == 0);
    assert(bb_vfs_is_layer_active(VFS_LAYER_BASE));
    assert(bb_vfs_is_layer_active(VFS_LAYER_MOD));
    assert(!bb_vfs_is_layer_active(VFS_LAYER_PATCH));

    /* 4. Test resolution with base file only */
    char base_file[2048];
    snprintf(base_file, sizeof(base_file), "%s/dvdroot_test.bin", base_dir);
    FILE *f1 = fopen(base_file, "w");
    assert(f1 != NULL);
    fputs("base_content", f1);
    fclose(f1);

    char resolved[2048] = {0};
    bool found = bb_vfs_resolve_path("/app0/dvdroot_test.bin", resolved, sizeof(resolved));
    assert(found);
    assert(strcmp(resolved, base_file) == 0);

    /* 5. Test mod overlay overriding base file */
    char mod_file[2048];
    snprintf(mod_file, sizeof(mod_file), "%s/dvdroot_test.bin", mod_dir);
    FILE *f2 = fopen(mod_file, "w");
    assert(f2 != NULL);
    fputs("mod_content", f2);
    fclose(f2);

    memset(resolved, 0, sizeof(resolved));
    found = bb_vfs_resolve_path("app0/dvdroot_test.bin", resolved, sizeof(resolved));
    assert(found);
    assert(strcmp(resolved, mod_file) == 0); /* Mod layer must override Base layer */

    /* 6. Unmount */
    bb_vfs_unmount(VFS_LAYER_MOD);
    assert(!bb_vfs_is_layer_active(VFS_LAYER_MOD));

    memset(resolved, 0, sizeof(resolved));
    found = bb_vfs_resolve_path("dvdroot_test.bin", resolved, sizeof(resolved));
    assert(found);
    assert(strcmp(resolved, base_file) == 0); /* After unmounting mod, resolves to Base */

    /* Clean up temp files */
    unlink(base_file);
    unlink(mod_file);
    rmdir(base_dir);
    rmdir(mod_dir);

    printf("PASS: Virtual File System layered resolution verified!\n");
    return 0;
}

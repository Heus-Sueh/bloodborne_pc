#define _GNU_SOURCE
#include "runtime_subsystems.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations of existing subsystem hooks where available */
extern void runtime_memory_reserve(void);
extern void runtime_memory_report(void);
extern void runtime_mutex_report(void);

/* Individual subsystem handlers */

static int kernel_subsystem_init(uint64_t capabilities) {
    (void)capabilities;
    return 0;
}

static void kernel_subsystem_shutdown(void) {
}

static void kernel_subsystem_stats(void) {
    runtime_mutex_report();
}

static int memory_subsystem_init(uint64_t capabilities) {
    (void)capabilities;
    runtime_memory_reserve();
    return 0;
}

static void memory_subsystem_shutdown(void) {
}

static void memory_subsystem_stats(void) {
    runtime_memory_report();
}

static int vfs_subsystem_init(uint64_t capabilities) {
    (void)capabilities;
    return 0;
}

static void vfs_subsystem_shutdown(void) {
}

static void vfs_subsystem_stats(void) {
}

static int input_subsystem_init(uint64_t capabilities) {
    (void)capabilities;
    return 0;
}

static void input_subsystem_shutdown(void) {
}

static void input_subsystem_stats(void) {
}

static int audio_subsystem_init(uint64_t capabilities) {
    (void)capabilities;
    return 0;
}

static void audio_subsystem_shutdown(void) {
}

static void audio_subsystem_stats(void) {
}

/* Master subsystem table */
static BbSubsystemContract g_subsystems[BB_SUBSYSTEM_COUNT] = {
    [BB_SUBSYSTEM_KERNEL] = {
        .name = "Kernel",
        .init = kernel_subsystem_init,
        .shutdown = kernel_subsystem_shutdown,
        .dump_stats = kernel_subsystem_stats,
        .status = BB_SUBSYSTEM_STATUS_UNINITIALIZED,
    },
    [BB_SUBSYSTEM_MEMORY] = {
        .name = "Memory",
        .init = memory_subsystem_init,
        .shutdown = memory_subsystem_shutdown,
        .dump_stats = memory_subsystem_stats,
        .status = BB_SUBSYSTEM_STATUS_UNINITIALIZED,
    },
    [BB_SUBSYSTEM_VFS] = {
        .name = "VirtualFileSystem",
        .init = vfs_subsystem_init,
        .shutdown = vfs_subsystem_shutdown,
        .dump_stats = vfs_subsystem_stats,
        .status = BB_SUBSYSTEM_STATUS_UNINITIALIZED,
    },
    [BB_SUBSYSTEM_INPUT] = {
        .name = "Input",
        .init = input_subsystem_init,
        .shutdown = input_subsystem_shutdown,
        .dump_stats = input_subsystem_stats,
        .status = BB_SUBSYSTEM_STATUS_UNINITIALIZED,
    },
    [BB_SUBSYSTEM_AUDIO] = {
        .name = "Audio",
        .init = audio_subsystem_init,
        .shutdown = audio_subsystem_shutdown,
        .dump_stats = audio_subsystem_stats,
        .status = BB_SUBSYSTEM_STATUS_UNINITIALIZED,
    },
};

int bb_subsystems_init(uint64_t capabilities) {
    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        BbSubsystemContract *sub = &g_subsystems[i];
        if (sub->init) {
            int res = sub->init(capabilities);
            if (res != 0) {
                fprintf(stderr, "Runtime Subsystem [%s] initialization failed with error: %d\n",
                        sub->name, res);
                sub->status = BB_SUBSYSTEM_STATUS_ERROR;
                /* Teardown already initialized subsystems in reverse order */
                for (int j = i - 1; j >= 0; --j) {
                    if (g_subsystems[j].shutdown &&
                        g_subsystems[j].status == BB_SUBSYSTEM_STATUS_INITIALIZED) {
                        g_subsystems[j].shutdown();
                        g_subsystems[j].status = BB_SUBSYSTEM_STATUS_SHUTDOWN;
                    }
                }
                return res;
            }
        }
        sub->status = BB_SUBSYSTEM_STATUS_INITIALIZED;
    }
    return 0;
}

void bb_subsystems_shutdown(void) {
    for (int i = BB_SUBSYSTEM_COUNT - 1; i >= 0; --i) {
        BbSubsystemContract *sub = &g_subsystems[i];
        if (sub->status == BB_SUBSYSTEM_STATUS_INITIALIZED) {
            if (sub->shutdown) {
                sub->shutdown();
            }
            sub->status = BB_SUBSYSTEM_STATUS_SHUTDOWN;
        }
    }
}

const BbSubsystemContract *bb_subsystem_get(BbSubsystemId id) {
    if ((int)id < 0 || id >= BB_SUBSYSTEM_COUNT) {
        return NULL;
    }
    return &g_subsystems[id];
}

void bb_subsystems_dump_all_stats(void) {
    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        BbSubsystemContract *sub = &g_subsystems[i];
        if (sub->status == BB_SUBSYSTEM_STATUS_INITIALIZED && sub->dump_stats) {
            sub->dump_stats();
        }
    }
}

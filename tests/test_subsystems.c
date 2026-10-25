#define _GNU_SOURCE
#include "runtime_subsystems.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Stubs for external symbols referenced in runtime_subsystems.c when tested standalone */
void runtime_memory_reserve(void) {}
void runtime_memory_report(void) {}
void runtime_mutex_report(void) {}

int main(void) {
    printf("[SUBSYSTEM TEST] Initializing...\n");

    /* 1. Pre-initialization checks */
    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        const BbSubsystemContract *sub = bb_subsystem_get((BbSubsystemId)i);
        assert(sub != NULL);
        assert(sub->name != NULL && strlen(sub->name) > 0);
        assert(sub->status == BB_SUBSYSTEM_STATUS_UNINITIALIZED);
    }
    assert(bb_subsystem_get((BbSubsystemId)-1) == NULL);
    assert(bb_subsystem_get(BB_SUBSYSTEM_COUNT) == NULL);

    /* 2. Initialize all subsystems */
    int init_res = bb_subsystems_init(0);
    assert(init_res == 0);

    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        const BbSubsystemContract *sub = bb_subsystem_get((BbSubsystemId)i);
        assert(sub != NULL);
        assert(sub->status == BB_SUBSYSTEM_STATUS_INITIALIZED);
    }

    /* 3. Dump diagnostic stats */
    bb_subsystems_dump_all_stats();

    /* 4. Shutdown subsystems */
    bb_subsystems_shutdown();

    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        const BbSubsystemContract *sub = bb_subsystem_get((BbSubsystemId)i);
        assert(sub != NULL);
        assert(sub->status == BB_SUBSYSTEM_STATUS_SHUTDOWN);
    }

    printf("PASS: Subsystem lifecycle contracts verified!\n");
    return 0;
}

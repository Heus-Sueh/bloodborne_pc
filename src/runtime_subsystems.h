#ifndef BB_RUNTIME_SUBSYSTEMS_H
#define BB_RUNTIME_SUBSYSTEMS_H

#ifdef __cplusplus
#include "runtime_subsystems.hpp"
#else

#include "platform/platform.h"
#include <stdbool.h>
#include <stdint.h>

/* Subsystem identifier enumeration */
typedef enum {
    BB_SUBSYSTEM_KERNEL = 0,
    BB_SUBSYSTEM_MEMORY,
    BB_SUBSYSTEM_VFS,
    BB_SUBSYSTEM_INPUT,
    BB_SUBSYSTEM_AUDIO,
    BB_SUBSYSTEM_SAVEDATA,
    BB_SUBSYSTEM_CONTENT,
    BB_SUBSYSTEM_RTC,
    BB_SUBSYSTEM_COUNT
} BbSubsystemId;

/* Subsystem lifecycle status */
typedef enum {
    BB_SUBSYSTEM_STATUS_UNINITIALIZED = 0,
    BB_SUBSYSTEM_STATUS_INITIALIZED,
    BB_SUBSYSTEM_STATUS_ERROR,
    BB_SUBSYSTEM_STATUS_SHUTDOWN
} BbSubsystemStatus;

/* Standard subsystem contract */
typedef struct {
    const char *name;
    int (*init)(uint64_t capabilities);
    void (*shutdown)(void);
    void (*dump_stats)(void);
    BbSubsystemStatus status;
} BbSubsystemContract;

/* Initializes all registered runtime subsystems in deterministic order. */
int bb_subsystems_init(uint64_t capabilities);

/* Shuts down all registered subsystems in reverse initialization order. */
void bb_subsystems_shutdown(void);

/* Retrieves the contract descriptor and status for a specific subsystem. */
const BbSubsystemContract *bb_subsystem_get(BbSubsystemId id);

/* Dumps diagnostic statistics across all active subsystems. */
void bb_subsystems_dump_all_stats(void);

#endif /* __cplusplus */

#endif /* BB_RUNTIME_SUBSYSTEMS_H */

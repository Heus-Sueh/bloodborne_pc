#pragma once

#include "platform/platform.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace bb::subsystems {

enum class SubsystemId : uint8_t {
    Kernel = 0,
    Memory,
    VFS,
    Input,
    Audio,
    Count
};

enum class SubsystemStatus : uint8_t {
    Uninitialized = 0,
    Initialized,
    Error,
    Shutdown
};

struct SubsystemContract {
    std::string_view name;
    std::function<int(uint64_t)> init;
    std::function<void()> shutdown;
    std::function<void()> dump_stats;
    SubsystemStatus status{SubsystemStatus::Uninitialized};
};

class SubsystemManager {
public:
    static SubsystemManager& Instance();

    int InitializeAll(uint64_t capabilities);
    void ShutdownAll();
    [[nodiscard]] const SubsystemContract* GetSubsystem(SubsystemId id) const;
    void DumpAllStats() const;

private:
    SubsystemManager();
    std::array<SubsystemContract, static_cast<size_t>(SubsystemId::Count)> subsystems_;
};

} // namespace bb::subsystems

/* C API Bridges */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_SUBSYSTEM_KERNEL = 0,
    BB_SUBSYSTEM_MEMORY,
    BB_SUBSYSTEM_VFS,
    BB_SUBSYSTEM_INPUT,
    BB_SUBSYSTEM_AUDIO,
    BB_SUBSYSTEM_COUNT
} BbSubsystemId;

typedef enum {
    BB_SUBSYSTEM_STATUS_UNINITIALIZED = 0,
    BB_SUBSYSTEM_STATUS_INITIALIZED,
    BB_SUBSYSTEM_STATUS_ERROR,
    BB_SUBSYSTEM_STATUS_SHUTDOWN
} BbSubsystemStatus;

typedef struct {
    const char *name;
    int (*init)(uint64_t capabilities);
    void (*shutdown)(void);
    void (*dump_stats)(void);
    BbSubsystemStatus status;
} BbSubsystemContract;

int bb_subsystems_init(uint64_t capabilities);
void bb_subsystems_shutdown(void);
const BbSubsystemContract *bb_subsystem_get(BbSubsystemId id);
void bb_subsystems_dump_all_stats(void);

#ifdef __cplusplus
}
#endif

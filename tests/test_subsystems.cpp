#include "runtime_subsystems.hpp"
#include <cassert>
#include <iostream>

/* Stubs for external symbols */
extern "C" {
void runtime_memory_reserve(void) {}
void runtime_memory_report(void) {}
void runtime_mutex_report(void) {}
}

int main() {
    std::cout << "[SUBSYSTEM C++23 TEST] Initializing...\n";

    auto& manager = bb::subsystems::SubsystemManager::Instance();

    // 1. C++23 API Check pre-init
    for (size_t i = 0; i < static_cast<size_t>(bb::subsystems::SubsystemId::Count); ++i) {
        const auto* sub = manager.GetSubsystem(static_cast<bb::subsystems::SubsystemId>(i));
        assert(sub != nullptr);
        assert(!sub->name.empty());
        assert(sub->status == bb::subsystems::SubsystemStatus::Uninitialized);
    }

    // 2. C API Check pre-init
    for (int i = 0; i < BB_SUBSYSTEM_COUNT; ++i) {
        const auto* sub = bb_subsystem_get(static_cast<BbSubsystemId>(i));
        assert(sub != nullptr);
        assert(sub->status == BB_SUBSYSTEM_STATUS_UNINITIALIZED);
    }

    // 3. Initialize
    assert(bb_subsystems_init(0) == 0);

    for (size_t i = 0; i < static_cast<size_t>(bb::subsystems::SubsystemId::Count); ++i) {
        const auto* sub = manager.GetSubsystem(static_cast<bb::subsystems::SubsystemId>(i));
        assert(sub != nullptr);
        assert(sub->status == bb::subsystems::SubsystemStatus::Initialized);
    }

    // 4. Dump stats
    bb_subsystems_dump_all_stats();

    // 5. Shutdown
    bb_subsystems_shutdown();

    for (size_t i = 0; i < static_cast<size_t>(bb::subsystems::SubsystemId::Count); ++i) {
        const auto* sub = manager.GetSubsystem(static_cast<bb::subsystems::SubsystemId>(i));
        assert(sub != nullptr);
        assert(sub->status == bb::subsystems::SubsystemStatus::Shutdown);
    }

    std::cout << "PASS: Subsystem C++23 lifecycle contracts verified!\n";
    return 0;
}

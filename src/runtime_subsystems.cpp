#include "runtime_subsystems.hpp"
#include "runtime_savedata.hpp"
#include <iostream>
#include <ranges>

/* Forward declarations of low-level C runtime hooks */
extern "C" {
void runtime_memory_reserve(void);
void runtime_memory_report(void);
void runtime_mutex_report(void);
}

namespace bb::subsystems {

SubsystemManager& SubsystemManager::Instance() {
    static SubsystemManager instance;
    return instance;
}

SubsystemManager::SubsystemManager() {
    subsystems_[static_cast<size_t>(SubsystemId::Kernel)] = SubsystemContract{
        .name = "Kernel",
        .init = [](uint64_t) { return 0; },
        .shutdown = []() {},
        .dump_stats = []() { runtime_mutex_report(); },
        .status = SubsystemStatus::Uninitialized,
    };

    subsystems_[static_cast<size_t>(SubsystemId::Memory)] = SubsystemContract{
        .name = "Memory",
        .init = [](uint64_t) {
            runtime_memory_reserve();
            return 0;
        },
        .shutdown = []() {},
        .dump_stats = []() { runtime_memory_report(); },
        .status = SubsystemStatus::Uninitialized,
    };

    subsystems_[static_cast<size_t>(SubsystemId::VFS)] = SubsystemContract{
        .name = "VirtualFileSystem",
        .init = [](uint64_t) { return 0; },
        .shutdown = []() {},
        .dump_stats = []() {},
        .status = SubsystemStatus::Uninitialized,
    };

    subsystems_[static_cast<size_t>(SubsystemId::Input)] = SubsystemContract{
        .name = "Input",
        .init = [](uint64_t) { return 0; },
        .shutdown = []() {},
        .dump_stats = []() {},
        .status = SubsystemStatus::Uninitialized,
    };

    subsystems_[static_cast<size_t>(SubsystemId::Audio)] = SubsystemContract{
        .name = "Audio",
        .init = [](uint64_t) { return 0; },
        .shutdown = []() {},
        .dump_stats = []() {},
        .status = SubsystemStatus::Uninitialized,
    };

    subsystems_[static_cast<size_t>(SubsystemId::SaveData)] = SubsystemContract{
        .name = "SaveData",
        .init = [](uint64_t) { return 0; },
        .shutdown = []() { bb::savedata::SaveDataManager::Instance().Reset(); },
        .dump_stats = []() { bb::savedata::SaveDataManager::Instance().Report(); },
        .status = SubsystemStatus::Uninitialized,
    };
}

int SubsystemManager::InitializeAll(uint64_t capabilities) {
    for (size_t i = 0; i < subsystems_.size(); ++i) {
        auto& sub = subsystems_[i];
        if (sub.init) {
            int res = sub.init(capabilities);
            if (res != 0) {
                std::cerr << "Subsystem [" << sub.name << "] failed init: " << res << '\n';
                sub.status = SubsystemStatus::Error;
                // Reverse teardown of initialized subsystems
                for (size_t j = i; j > 0; --j) {
                    auto& prev = subsystems_[j - 1];
                    if (prev.shutdown && prev.status == SubsystemStatus::Initialized) {
                        prev.shutdown();
                        prev.status = SubsystemStatus::Shutdown;
                    }
                }
                return res;
            }
        }
        sub.status = SubsystemStatus::Initialized;
    }
    return 0;
}

void SubsystemManager::ShutdownAll() {
    for (auto& sub : std::ranges::reverse_view(subsystems_)) {
        if (sub.status == SubsystemStatus::Initialized) {
            if (sub.shutdown) {
                sub.shutdown();
            }
            sub.status = SubsystemStatus::Shutdown;
        }
    }
}

const SubsystemContract* SubsystemManager::GetSubsystem(SubsystemId id) const {
    const auto idx = static_cast<size_t>(id);
    if (idx >= subsystems_.size()) {
        return nullptr;
    }
    return &subsystems_[idx];
}

void SubsystemManager::DumpAllStats() const {
    for (const auto& sub : subsystems_) {
        if (sub.status == SubsystemStatus::Initialized && sub.dump_stats) {
            sub.dump_stats();
        }
    }
}

} // namespace bb::subsystems

/* --------------------------------------------------------------------------
 * C API Bridges
 * -------------------------------------------------------------------------- */

extern "C" {

static BbSubsystemContract g_c_subsystem_cache[BB_SUBSYSTEM_COUNT];

int bb_subsystems_init(uint64_t capabilities) {
    return bb::subsystems::SubsystemManager::Instance().InitializeAll(capabilities);
}

void bb_subsystems_shutdown(void) {
    bb::subsystems::SubsystemManager::Instance().ShutdownAll();
}

const BbSubsystemContract *bb_subsystem_get(BbSubsystemId id) {
    if (id < 0 || id >= BB_SUBSYSTEM_COUNT) {
        return nullptr;
    }
    const auto* cpp_sub = bb::subsystems::SubsystemManager::Instance().GetSubsystem(
        static_cast<bb::subsystems::SubsystemId>(id));
    if (!cpp_sub) {
        return nullptr;
    }

    auto& c_sub = g_c_subsystem_cache[id];
    c_sub.name = cpp_sub->name.data();
    c_sub.status = static_cast<BbSubsystemStatus>(cpp_sub->status);
    return &c_sub;
}

void bb_subsystems_dump_all_stats(void) {
    bb::subsystems::SubsystemManager::Instance().DumpAllStats();
}

}

// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame & memory telemetry counters (BB_FRAME_STATS=1).
// Split out of bbport_toggles.h so toggle-only translation units do not pull in telemetry.
#pragma once

#include "shim/bb_env.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace BbStats {

/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
/// Writes the GPU side heard of from the writers (libc copies over watched pages, no write tracking).
inline std::atomic<std::uint64_t> cpu_write_notes{0}, cpu_write_note_bytes{0};
inline std::atomic<std::uint64_t> fast_writer_faults{0}; ///< write faults of the game's bookkeeping, no pipeline wait
/// Ranges the game's GPU memory allocator handed out (guest hook).
inline std::atomic<std::uint64_t> gpu_range_allocs{0}, gpu_range_alloc_bytes{0};
/// Buffer bindings: bytes bound where the game's memory is (read over PCIe on a
/// discrete GPU) and bytes bound to VRAM copies.
inline std::atomic<std::uint64_t> bound_in_place_bytes{0}, bound_vram_bytes{0};
inline std::atomic<std::uint64_t> bound_in_place_written_bytes{0};
/// Vertex and index data of the draws, by where the GPU reads it: [0] in place, [1] VRAM copy / stream.
inline std::atomic<std::uint64_t> vertex_bytes[2]{}, index_bytes[2]{}, geometry_draws{0};
inline std::atomic<std::uint64_t> gpu_data_updates{0}, copied_back_blocks{0}, readbacks{0};
inline std::atomic<std::uint64_t> late_vram_writes{0};
inline std::atomic<std::uint64_t> shadow_copies{0}, shadow_bytes{0};
inline std::atomic<std::uint64_t> write_shadow_copies{0}, write_shadow_bytes{0}, write_shadow_hits{0},
    write_shadow_misses{0};
inline std::atomic<std::uint64_t> idle_flushes{0};
inline std::atomic<std::uint64_t> drain_flushes{0};
inline std::atomic<std::uint64_t> eop_decoded{0}, eop_written{0}, gpu_labels{0};
inline std::atomic<std::uint64_t> submits_allowed_queries{0}, submits_refused{0}, submit_done_calls{0};
inline std::atomic<std::uint64_t> gnm_frame_waits{0}, gnm_frame_wait_ns{0};
inline std::atomic<std::uint64_t> asc_submits{0};
inline std::atomic<std::uint64_t> guest_submit_stamp{0}, started_submit_stamp{0};
inline std::array<std::atomic<std::uint64_t>, 4> range_allocators{}, range_live{}, range_bytes{};
inline std::atomic<std::int64_t> hot_pages{0};
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1};
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
inline std::atomic<std::uint64_t> gpu_frames{0};
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
inline std::array<std::atomic<std::uint64_t>, 12> t_layer{};
inline std::atomic<std::uint64_t> layer_entries_written{0};
inline std::atomic<std::uint64_t> layer_mprotects{0};

/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    return bb_env_on("BB_FRAME_STATS");
}();

/// Single parameterized RAII nanosecond timer replacing three near-duplicate timer structs.
template <bool GatedByEnabled>
struct ScopedTimer {
    std::atomic<std::uint64_t>& total;
    std::atomic<std::uint64_t>* count = nullptr;
    std::chrono::steady_clock::time_point start =
        (!GatedByEnabled || enabled) ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};

    explicit ScopedTimer(std::atomic<std::uint64_t>& t) : total(t) {}
    ScopedTimer(std::atomic<std::uint64_t>& t, std::atomic<std::uint64_t>& c) : total(t), count(&c) {}

    ~ScopedTimer() {
        if constexpr (GatedByEnabled) {
            if (!enabled) return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
        if (count) {
            count->fetch_add(1, std::memory_order_relaxed);
        }
    }
};

using Timer = ScopedTimer<true>;
using WaitTimer = ScopedTimer<false>;
using VkMemTimer = ScopedTimer<false>;

inline std::atomic<std::uint64_t> gpu_idle_ns{0};
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0}, host_copy_waits_skipped{0};
inline std::atomic<std::uint64_t> present_wait_ns{0};
inline std::atomic<std::uint64_t> frame_pool_wait_ns{0};
inline std::atomic<std::uint64_t> present_call_ns{0};
inline std::atomic<std::uint32_t> frame_number{0};
inline std::atomic<std::uint64_t> device_alloc_bytes{0}, device_free_bytes{0};
inline std::atomic<std::uint64_t> live_image_bytes{0}, live_images{0};
inline std::atomic<std::uint64_t> vk_image_bytes{0};
inline std::atomic<std::uint64_t> t_vk_alloc{0}, t_vk_free{0}, n_vk_alloc{0}, n_vk_free{0};
inline std::atomic<std::uint64_t> t_vk_submit{0}, n_vk_submit{0};
inline std::atomic<std::uint64_t> t_img_create{0}, t_img_alloc{0}, t_img_bind{0};
inline std::atomic<std::uint64_t> mirror_new_bytes{0}, image_new_bytes{0}, staging_new_bytes{0};
inline std::atomic<std::uint64_t> mirror_new_by[3]{};
inline thread_local int mirror_new_reason = 0;
inline std::atomic<std::uint64_t> residency_alloc_bytes{0}, residency_unused_bytes{0};
inline std::atomic<std::uint64_t> gc_used_bytes{0}, gc_trigger_bytes{0}, gc_freed_images{0};
inline std::atomic<std::uint64_t> gc_critical_bytes{0};
inline std::atomic<bool> mirror_copies_waiting{false};
inline std::atomic<std::uint32_t> vram_budget_lost_mb{0};
inline std::atomic<std::uint32_t> coarse_second{0};
inline std::atomic<std::uint64_t> vram_idle_bytes{0}, vram_unmapped_bytes{0}, vram_chunks_freed_bytes{0};
inline std::atomic<std::uint64_t> dynamic_reloaded_bytes{0};
inline std::atomic<std::uint64_t> gpu_writes_kept_by_cpu_bytes{0};
inline std::atomic<std::uint64_t> residency_chunk_count{0}, evacuations{0}, evac_kept_gpu{0}, evac_kept_other{0};
inline std::atomic<std::uint64_t> gc_kept_images{0};
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};

} // namespace BbStats

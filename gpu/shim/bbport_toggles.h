// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
// Telemetry counters live separately in bbport_stats.h to keep toggle-only TUs lean.
#pragma once

#include <array>
#include <cstdint>

#include "runtime_gpu_abi.h"

namespace BbToggle {

enum : std::uint64_t {
    RegionCache = 1ull << 0,
    FetchShaderCache = 1ull << 1,
    PageTrackingEarlyExit = 1ull << 2,
    PendingPollLimit = 1ull << 3,
    ThreadedRecording = 1ull << 4,
    ImageDescCache = 1ull << 5,
    LockFreeUploadCheck = 1ull << 6,
    FindImageCache = 1ull << 7,
    DeferredUploads = 1ull << 8,
    /// Frame generation hudless pass (moved off colliding bit 30 onto retired AccessMemo bit 9).
    FrameGenHudless = 1ull << 9,
    TextureBindingMemo = 1ull << 10,
    CoarseReadTracking = 1ull << 11,
    PreparedResources = 1ull << 12,
    DrawPreparation = 1ull << 13,
    DeferredStreamCopies = 1ull << 14,
    HotPages = 1ull << 15,
    FaultWindow = 1ull << 16,
    ParallelCopies = 1ull << 17,
    /// Clears of proxied scene images clear the proxy too instead of resolving it first
    /// (moved off colliding bit 20 onto bit 18).
    SceneClearProxies = 1ull << 18,
    PoolSmallCopies = 1ull << 19,

    // Camera / object motion & temporal upscaler debug toggles (bits 20-29).
    CameraMotionMode1 = 1ull << 20,
    CameraMotionMode2 = 1ull << 21,
    CameraMotionMode3 = 1ull << 22,
    CameraMotionMode4 = 1ull << 23,
    UpscalerObjectMotion = 1ull << 24,
    UpscalerJitter = 1ull << 25,
    UpscalerInvertJitterSign = 1ull << 26,
    UpscalerReactive = 1ull << 27,
    UpscalerDebugBit28 = 1ull << 28,
    ObjectMotionDraws = 1ull << 29,

    UpdateImageFastPath = 1ull << 30,
    /// Bit set: the texture collector frees nothing (A/B of its evictions while the game runs).
    TextureCollector = 1ull << 31,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    /// Copy of a reduced scene target copies its proxy (SceneTargets::CopyProxy).
    SceneProxyCopies = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    /// Copy shader into memory that is no image yet makes the image there.
    ComputeCopyNewImage = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    TaaKeepNearerHistory = 1ull << 55,
    Fsr4MotionYFlip = 1ull << 56,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    ParallelRecording = 1ull << 58,
    ReadbackQueue = 1ull << 59,
    CameraOwnInverse = 1ull << 60,
    ForcedAniso = 1ull << 61,
    CameraYFlip = 1ull << 62,
    Fsr4EncodedInput = 1ull << 63,
};

namespace detail {
inline constexpr std::array<std::uint64_t, 64> AllToggleBits = {
    RegionCache, FetchShaderCache, PageTrackingEarlyExit, PendingPollLimit,
    ThreadedRecording, ImageDescCache, LockFreeUploadCheck, FindImageCache,
    DeferredUploads, FrameGenHudless, TextureBindingMemo, CoarseReadTracking,
    PreparedResources, DrawPreparation, DeferredStreamCopies, HotPages,
    FaultWindow, ParallelCopies, SceneClearProxies, PoolSmallCopies,
    CameraMotionMode1, CameraMotionMode2, CameraMotionMode3, CameraMotionMode4,
    UpscalerObjectMotion, UpscalerJitter, UpscalerInvertJitterSign, UpscalerReactive,
    UpscalerDebugBit28, ObjectMotionDraws, UpdateImageFastPath, TextureCollector,
    RecordPrefetch, TextureViewMemo, TextureBindHelper, EarlyDrawInputs,
    ConstantRing, DrawPipeline, PipelinedTasks, PendingFenceWaits,
    PipelinedDispatch, SceneProxyCopies, PipelinedMemoryWrites, MultiCopyShader,
    RenderStateMemo, TextureSetMemo, PipelinedIndirectDraws, SceneAttachmentsOnly,
    SampleSceneProxies, ComputeCopyNewImage, SceneHalfRes, TaaTonemapBlend,
    TaaClip, TaaVariance, TaaFilter, TaaKeepNearerHistory,
    Fsr4MotionYFlip, SceneMipBias, ParallelRecording, ReadbackQueue,
    CameraOwnInverse, ForcedAniso, CameraYFlip, Fsr4EncodedInput,
};

consteval bool AllToggleBitsUnique() {
    std::uint64_t seen = 0;
    for (std::uint64_t bit : AllToggleBits) {
        if (bit == 0 || (bit & (bit - 1)) != 0) return false;
        if ((seen & bit) != 0) return false;
        seen |= bit;
    }
    return true;
}
static_assert(AllToggleBitsUnique(), "BbToggle bits must be single-bit powers of two with zero collisions");
} // namespace detail

inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}

/// A temporary experiment switched on by bit `bit` of the second number in BB_TOGGLE_FILE.
inline bool Experiment(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_experiment_bits, __ATOMIC_RELAXED) & bit) != 0;
}

} // namespace BbToggle

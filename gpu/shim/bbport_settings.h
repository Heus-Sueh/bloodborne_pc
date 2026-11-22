// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Persisted preferences (Values), GPU/driver capabilities (Capabilities), live frame
// readouts (FrameTelemetry), and startup snapshots (StartupSnapshot) live behind separate
// accessors so each concern stays cohesive.

#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerFsr1 = 6, UpscalerFsrAmd = 7,
                      UpscalerCount };
/// FSR 1 (EASU + RCAS): spatial, no history or motion vectors; the cheapest (weak GPUs).
inline bool IsSpatial(int upscaler) {
    return upscaler == UpscalerFsr1;
}
/// FSR 4 v07, FSR 4.1.1 or AMD's own FSR DLLs (upscale/vk_fsr_amd).
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411 || upscaler == UpscalerFsrAmd;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum FrameGen : int {
    FrameGenOff = 0,
    FrameGenFsr3 = 1,
    FrameGenDlss = 2,
    FrameGenFsrAmd = 3, ///< AMD's frame generation DLL (Windows; FSR 4.0.2 ML on RDNA4)
    FrameGenCount
};
enum LowLatencyMode : int { LowLatencyOff = 0, LowLatencyOn = 1, LowLatencyBoost = 2, LowLatencyCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugAo = 3, DebugViewCount };
enum class MenuLanguage { English, Russian };

/// Strongly-typed indices into Effects[] and Values::effects[] (eliminates raw magic indices).
enum EffectId : int {
    EffectChromaticAberration = 0,
    EffectDof = 1,
    EffectMotionBlur = 2,
    EffectSsao = 3,
    EffectGameAa = 4,
    EffectDynamicShadows = 5,
    EffectSsr = 6,
    EffectBloom = 7,
    EffectSkipIntro = 8,
    EffectDebugCamera = 9,
    EffectDebugMenu = 10,
    EffectCount = 11,
};

/// Game effects switched by the community patches at start (patches.py EFFECTS).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[EffectCount] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"effect_bloom", "Bloom", "Свечение (Bloom)", true},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
/// The game's own anti-aliasing effect (Effects index), derived from anti_aliasing.
inline constexpr int GameAaEffect = EffectGameAa;

enum AntiAliasing : int { AaAuto = 0, AaGame = 1, AaFxaa = 2, AaOff = 3, AaSmaa = 4, AaCount };
const char* AntiAliasingName(int mode);

enum TextureQuality : int { TqAuto = 0, TqFull = 1, TqHalf = 2, TqQuarter = 3, TqCount };
const char* TextureQualityName(int quality);

enum Fsr4Model : int {
    Fsr4Auto = 0, Fsr4Fp8 = 1, Fsr4Int8 = 2, Fsr4Rdna2 = 3, Fsr4Lite = 4, Fsr4ModelCount
};
const char* Fsr4ModelName(int model);

/// SSR changes resource dimensions: the complete patch must precede construction.
inline constexpr bool IsHotEffect(int effect) {
    return effect >= 0 && effect < EffectSkipIntro && effect != EffectSsr;
}

enum Aspect : int { AspectAuto = 0, Aspect4x3, Aspect16x10, Aspect16x9, Aspect21x9, Aspect32x9, AspectCount };
struct OutputSize {
    int width = 1920, height = 1080;
    bool operator==(const OutputSize&) const = default;
};
inline constexpr OutputSize OutputDefault{1920, 1080}; ///< the game's own size
constexpr std::uint32_t PackSize(OutputSize s) {
    return std::uint32_t(s.width) | std::uint32_t(s.height) << 16;
}
constexpr OutputSize UnpackSize(std::uint32_t v) {
    return {int(v & 0xffff), int(v >> 16)};
}
/// An aspect ratio's outputs, smallest first.
struct OutputList {
    std::array<OutputSize, 12> sizes{};
    int count = 0;
    const OutputSize* begin() const { return sizes.data(); }
    const OutputSize* end() const { return sizes.data() + count; }
    int IndexOf(OutputSize size) const; ///< -1: not in the list
};
const char* AspectKey(int aspect);
void SetMonitor(int width, int height);
OutputSize Monitor();
int ResolvedAspect(int aspect);
OutputList Outputs(int aspect);
OutputSize NearestOutput(int aspect, OutputSize size);
OutputSize CurrentOutput();
std::string SizeText(OutputSize size);

/// Persisted Game Settings loaded from bbport.ini and edited live by the in-game menu.
struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::Russian};
    std::atomic<bool> menu_language_chosen{false};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    std::atomic<bool> vbao{false};
    alignas(16) std::atomic<float> camera_speed[4]{1.0f, 1.0f, 1.0f, 1.0f};
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0};
    std::atomic<bool> light_grid_opt{false};
    std::atomic<int> anti_aliasing{AaAuto};
    std::atomic<int> texture_quality{TqAuto};
    std::atomic<int> fsr4_model{Fsr4Auto};
    std::atomic<bool> fxaa_forced{false};
    std::atomic<int> aspect{AspectAuto};
    std::atomic<std::uint32_t> output_res{PackSize(OutputDefault)};
    std::atomic<bool> output_auto{true};
    std::atomic<int> live_resolution{1};
    std::atomic<int> frame_generation{FrameGenOff};
    std::atomic<int> frame_generation_multiplier{2};
    std::atomic<int> low_latency{LowLatencyOn};
};

/// Renderer-owned GPU/driver capability flags and diagnostic messages (never persisted).
struct Capabilities {
    std::atomic<unsigned> fsr4_models_available{0};
    std::atomic<const char*> fsr4_model_active{nullptr};
    /// The FSR version of that model ("FSR 4.1.2"), for the menu and the FPS line; null before.
    std::atomic<const char*> fsr4_version_active{nullptr};
    std::atomic<bool> frame_generation_supported{false};
    std::atomic<const char*> frame_generation_problem{nullptr};
    std::atomic<bool> frame_generation_restart{false};
    std::atomic<bool> scene_proxies{true};
    std::atomic<int> dlssg_max_multiplier{1};
    std::atomic<const char*> dlssg_problem{nullptr};
    std::atomic<bool> low_latency_supported{false};
    std::atomic<int> low_latency_backend{0};
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    std::atomic<bool> fsr_amd_supported{false};
    std::atomic<const char*> fsr_amd_provider{nullptr};
    std::atomic<unsigned> fsr_amd_models{0};
    std::atomic<bool> fg_amd_supported{false};
    std::atomic<const char*> fg_amd_provider{nullptr};
    std::atomic<const char*> fg_amd_problem{nullptr};
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
};

/// Live per-frame presenter and upscaler readouts for the overlay (never persisted).
struct FrameTelemetry {
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    std::atomic<float> game_fps{0.0f};
    std::atomic<int> frame_gen_now{0};
};

/// Immutable snapshot captured at Load() describing the patches applied at process start.
struct StartupSnapshot {
    int preset = NativeAA;
    int upscaler = UpscalerFsr3;
    bool object_motion = true;
    bool effects[EffectCount]{};
    int model_lod = 0;
    OutputSize output{0, 0};
    int live_resolution = 0;
};

Values& Get();
Capabilities& Caps();
FrameTelemetry& Telemetry();
StartupSnapshot& Startup();

const char* MenuText(const char* english, const char* russian);

void Load();
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
void ConfigureFsrAmdSupport(bool available);
void ConfigureDlssSupport(bool available, const char* problem);
bool FixedRenderSession();
void SyncAntiAliasing();
bool HybridSession();
int RenderPreset();
bool ResolutionNeedsRestart();
void Save();

float PresetScale(int preset);
const char* PresetName(int preset);
const char* FrameGenName(int frame_gen);
const char* LowLatencyName(int mode);
int EffectiveLowLatency();
const char* UpscalerName(int upscaler);

} // namespace BbSettings

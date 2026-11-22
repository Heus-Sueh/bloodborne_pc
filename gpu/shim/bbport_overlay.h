// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "common/types.h"
#include "vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;
struct ImTextureData;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();
/// A line of text over the game until called with null (bbgpu_overlay_banner).
void SetBanner(const char* text);
/// A trophy unlocked (bbport_trophies.cpp): announced in PlayStation 4's top-left notification
/// style for a few seconds, one after another. grade: P G S B, '?' unknown; icon: the trophy's PNG
/// (empty: a metallic trophy cup tile). delay: seconds of drawn frames before it shows
/// (BB_TROPHY_TEST: past the game's start).
void ShowTrophy(char grade, const std::string& name, const std::filesystem::path& icon, float delay = 0.0f);

/// Snapshot of a trophy entry for the in-game PS4 Quick Menu (RmlUi).
struct TrophyEntryView {
    int id = 0;
    char grade = 'B'; // P, G, S, B, ?
    bool hidden = false;
    bool unlocked = false;
    std::string name;
    std::string detail;
    std::string unlocked_at;
    std::filesystem::path icon_path;
};

/// Returns the current trophy catalog with unlock status and icon paths (thread-safe).
std::vector<TrophyEntryView> GetTrophyCatalogSnapshot();

/// Triggers an immediate in-game PlayStation trophy notification preview (without recording).
void TriggerTrophyPreview(char grade = 'P');

/// Trophy notification visual style: PlayStation 4 (Black or Classic White) or PlayStation 5.
enum class TrophyStyle : int {
    Ps4Black = 0,
    Ps4White = 1,
    Ps5 = 2,
};

TrophyStyle GetTrophyStyle();
void SetTrophyStyle(TrophyStyle style);

/// PlayStation 4 trophy pop-up color theme (false = PS4 FW 5.0+ Black, true = PS4 Classic White).
bool GetTrophyLightTheme();
void SetTrophyLightTheme(bool light);

/// Loads or retrieves a cached RGBA32 PNG texture registered with the active ImGui Vulkan backend.
ImTextureData* GetOrLoadTexture(const std::filesystem::path& path);

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu or the text dialog is open: the game's input is held neutral.
bool CapturesInput();

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay

// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game PlayStation 4 Quick Menu built with RmlUi (rendered over the Vulkan swapchain
// via BbOverlay). Toggled with the gamepad Home / PS Guide button (SDL_GAMEPAD_BUTTON_GUIDE) or F1.

#pragma once

#include "common/types.h"

union SDL_Event;

namespace BbQuickMenu {

/// Whether the RmlUi PS4 Quick Menu is currently open.
bool IsOpen();

/// Opens or closes the Quick Menu.
void SetOpen(bool open);

/// Toggles the Quick Menu open/closed.
void Toggle();

/// Window thread: processes an SDL event; returns true when consumed by the Quick Menu.
bool HandleEvent(const SDL_Event& event);

/// Present thread (under BbOverlay's ImGui lock, between ImGui::NewFrame and ImGui::Render):
/// updates the RmlUi context and emits its geometry into ImGui::GetForegroundDrawList().
void Render(u32 display_w, u32 display_h, float dt);

} // namespace BbQuickMenu

// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "launcher_core.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <string>
#include <vector>

struct SDL_Window;

namespace bbport {

// Thin single-threaded RmlUi + SDL3 view adapter over LauncherCore.
// Contains zero business logic, zero file I/O, zero threads, and zero POSIX calls.
class LauncherModel {
public:
  LauncherModel(Rml::Context* context, SDL_Window* window, LauncherCore& core);

  void Update();
  bool ShouldClose() const noexcept { return should_close_; }

private:
  void BindModel();
  void ApplyDelta(const StateDelta& delta);
  void SyncModsAndPatches();
  void SyncTrophies();
  void SyncSaves();
  void SyncTranslations();

  void ChooseFolderForSetting(const char* setting_key);
  void ChooseDumpToExtract();
  void ChooseUpdateToInstall();
  void ChooseSaveLogPath();
  void ChooseSaveFolderToImport();

  Rml::Context* context_ = nullptr;
  SDL_Window* window_ = nullptr;
  LauncherCore& core_;
  Rml::DataModelHandle model_handle_;

  // Pure UI state & RmlUi array backing mirrors.
  std::string active_tab_ = "play";
  std::string copy_btn_text_ = "COPY";
  bool should_close_ = false;

  std::vector<ModItem> mods_view_;
  std::vector<PatchItem> patches_view_;
  std::vector<TrophyItem> trophies_view_;
  std::vector<SaveSlotItem> save_slots_view_;
  std::vector<SaveBackupItem> save_backups_view_;

  std::string tr_tab_play_;
  std::string tr_tab_graphics_;
  std::string tr_tab_effects_;
  std::string tr_tab_controls_;
  std::string tr_tab_mods_;
  std::string tr_tab_saves_;
  std::string tr_tab_trophies_;
  std::string tr_tab_online_;
  std::string tr_tab_log_;
  std::string tr_launch_game_;
  std::string tr_stop_game_;
};

} // namespace bbport

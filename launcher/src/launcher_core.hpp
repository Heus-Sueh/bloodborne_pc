// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "clock.hpp"
#include "content_stores.hpp"
#include "launch_env.hpp"
#include "port_paths.hpp"
#include "process_runner.hpp"
#include "settings_store.hpp"
#include "system_probe.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bbport {

struct StateDelta {
  std::vector<std::string> dirty_keys;
  bool focus_log_tab = false;

  bool Empty() const noexcept { return dirty_keys.empty() && !focus_log_tab; }
  bool Contains(std::string_view key) const noexcept;
  void Mark(std::string key);
  void Merge(const StateDelta& other);
};

struct ActionResult {
  bool ok = true;
  std::string error;
  StateDelta delta;

  explicit operator bool() const noexcept { return ok; }
};

struct ProcessState {
  bool running = false;
  std::optional<int> last_exit_code;
};

struct ExtractionState {
  bool active = false;
  int progress_pct = 0;
  std::string status_text;
};

struct LauncherAdapters {
  std::unique_ptr<ProcessRunner> process_runner; // null -> PosixProcessRunner
  std::unique_ptr<SystemProbe> system_probe;     // null -> LinuxSystemProbe
  std::unique_ptr<Clock> clock;                  // null -> SteadyClock
};

// Deep headless Launcher Core module behind injected seams.
// UI-thread owned except Post* inbox methods (safe from SDL dialog callbacks).
class LauncherCore {
public:
  static ActionResult OpenDefault(std::unique_ptr<LauncherCore>& out,
                                  std::optional<std::filesystem::path> executable_path = std::nullopt);
  static ActionResult Open(PortPaths paths,
                           LauncherAdapters adapters,
                           std::unique_ptr<LauncherCore>& out);

  ~LauncherCore();
  LauncherCore(LauncherCore&&) noexcept;
  LauncherCore& operator=(LauncherCore&&) noexcept;
  LauncherCore(const LauncherCore&) = delete;
  LauncherCore& operator=(const LauncherCore&) = delete;

  // Settings & schema (single generic mutation path).
  const SettingsSchema& Schema() const noexcept;
  const SettingsStore& Settings() const noexcept;
  ActionResult SetSetting(std::string_view key, SettingValue value);
  ActionResult SaveSettings();

  // Launch environment & process lifecycle.
  LaunchPlan CompileLaunchPlan() const;
  ActionResult LaunchGame();
  ActionResult StopGame();
  ActionResult StartExtraction(const std::filesystem::path& dump_path,
                               const std::filesystem::path& output_dir);
  ActionResult StartUpdateInstall(const std::filesystem::path& update_pkg_path);

  // Mods & External Patches.
  ActionResult ToggleMod(std::string_view mod_id);
  ActionResult MoveMod(std::string_view mod_id, int delta_index);
  ActionResult TogglePatch(std::string_view patch_id);

  // Refreshes & logs.
  ActionResult RefreshGameStatus();
  ActionResult RefreshContent();
  ActionResult RefreshTrophies();
  ActionResult ClearLog();
  ActionResult SaveLog(const std::filesystem::path& dest_path);

  // Thread-safe cross-thread inbox (safe from SDL file/folder dialog callbacks).
  void PostSetting(std::string key, SettingValue value);
  void PostExtraction(std::filesystem::path dump_path, std::filesystem::path output_dir);
  void PostUpdateInstall(std::filesystem::path update_pkg_path);
  void PostSaveLog(std::filesystem::path dest_path);

  // Per-frame UI-thread pump: drains inbox, child output, and non-blocking process exits.
  StateDelta Poll();

  // Read-only state projections.
  const PortPaths& Paths() const noexcept;
  const GameInspection& GameStatus() const noexcept;
  const std::vector<ModItem>& Mods() const noexcept;
  const std::vector<PatchItem>& Patches() const noexcept;
  const std::vector<TrophyItem>& Trophies() const noexcept;
  const TrophyStats& TrophySummary() const noexcept;
  std::string TrophyProgressText() const;
  const ProcessState& GameProcess() const noexcept;
  const ExtractionState& Extraction() const noexcept;
  const std::string& ConsoleLog() const noexcept;
  std::string UiLanguage() const;
  const std::string& Translate(const std::string& key) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit LauncherCore(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace bbport

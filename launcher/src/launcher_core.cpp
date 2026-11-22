// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "launcher_core.hpp"
#include "i18n.hpp"
#include "native_launch_planner.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <variant>

namespace fs = std::filesystem;

namespace bbport {

namespace {

constexpr std::size_t kMaxConsoleLogBytes = 65536;
constexpr auto kQuickCrashThreshold = std::chrono::seconds(4);

struct PostSetSettingCmd {
  std::string key;
  SettingValue value;
};
struct PostExtractionCmd {
  fs::path dump_path;
  fs::path output_dir;
};
struct PostUpdateInstallCmd {
  fs::path update_pkg_path;
};
struct PostSaveLogCmd {
  fs::path dest_path;
};
struct PostImportSaveFolderCmd {
  fs::path external_dir;
};

using InboxCommand = std::variant<PostSetSettingCmd, PostExtractionCmd, PostUpdateInstallCmd, PostSaveLogCmd, PostImportSaveFolderCmd>;

} // namespace

bool StateDelta::Contains(std::string_view key) const noexcept {
  return std::any_of(dirty_keys.begin(), dirty_keys.end(), [&](const std::string& k) { return k == key; });
}

void StateDelta::Mark(std::string key) {
  if (!Contains(key)) {
    dirty_keys.push_back(std::move(key));
  }
}

void StateDelta::Merge(const StateDelta& other) {
  for (const auto& k : other.dirty_keys) {
    Mark(k);
  }
  focus_log_tab = focus_log_tab || other.focus_log_tab;
}

struct LauncherCore::Impl {
  PortPaths paths;
  std::unique_ptr<ProcessRunner> process_runner;
  std::unique_ptr<SystemProbe> system_probe;
  std::unique_ptr<Clock> clock;
  std::unique_ptr<SettingsStore> store;
  I18n i18n;

  GameInspection game_inspection;
  std::vector<ModItem> mods;
  std::vector<PatchItem> patches;
  TrophyCatalog trophy_catalog;

  SaveSummary save_summary;
  std::vector<SaveSlotItem> save_slots;
  std::vector<SaveBackupItem> save_backups;
  std::string save_feedback;

  ProcessState game_state;
  std::unique_ptr<ProcessSession> game_session;
  std::chrono::steady_clock::time_point game_launch_time{};
  std::optional<fs::path> active_mod_overlay;

  ExtractionState extraction_state;
  std::unique_ptr<ProcessSession> extract_session;
  fs::path pending_extract_out_dir;

  std::string console_log;

  std::mutex inbox_mutex;
  std::vector<InboxCommand> inbox;

  void AppendLog(std::string_view line, StateDelta& delta) {
    console_log.append(line);
    console_log.push_back('\n');
    if (console_log.size() > kMaxConsoleLogBytes) {
      console_log.erase(0, console_log.size() - kMaxConsoleLogBytes);
    }
    delta.Mark("console_log");
  }

  void MarkGameStatusDirty(StateDelta& delta) {
    delta.Mark("is_valid");
    delta.Mark("is_update_needed");
    delta.Mark("game_version");
    delta.Mark("game_title_id");
    delta.Mark("status_text");
  }

  void MarkTranslationsDirty(StateDelta& delta) {
    delta.Mark("ui_language");
    delta.Mark("translations");
    delta.Mark("status_text");
  }

  void MarkTrophiesDirty(StateDelta& delta) {
    delta.Mark("trophies_list");
    delta.Mark("trophy_progress_pct");
    delta.Mark("trophy_progress_text");
  }

  void MarkSavesDirty(StateDelta& delta) {
    delta.Mark("save_has_saves");
    delta.Mark("save_active_title_id");
    delta.Mark("save_total_size");
    delta.Mark("save_last_modified");
    delta.Mark("save_character_slots");
    delta.Mark("save_backup_count");
    delta.Mark("save_sound_ok");
    delta.Mark("save_status_line");
    delta.Mark("save_action_feedback");
    delta.Mark("save_slots_list");
    delta.Mark("save_backups_list");
  }

  void MarkExtractionDirty(StateDelta& delta) {
    delta.Mark("is_extracting");
    delta.Mark("extract_progress_pct");
    delta.Mark("extract_progress_text");
  }

  void RefreshGameStatusInternal(StateDelta& delta) {
    game_inspection = InspectGameDirectory(store->ResolvedGameDir());
    MarkGameStatusDirty(delta);
  }

  void RefreshContentInternal(StateDelta& delta) {
    mods = LoadMods(store->ResolvedModsDir(), paths.ModsConfigPath());
    patches = LoadExternalPatches(store->ResolvedPatchesDir(), paths.PatchesConfigPath(), paths.BuiltinPatchesXmlPath());
    delta.Mark("mods_list");
    delta.Mark("patches_list");
  }

  void RefreshTrophiesInternal(StateDelta& delta) {
    trophy_catalog = LoadTrophies(store->ResolvedSavesDir(), store->GetString("language", "1"));
    MarkTrophiesDirty(delta);
  }

  void RefreshSavesInternal(StateDelta& delta) {
    save_summary = InspectSaves(store->ResolvedSavesDir(), save_slots, save_backups);
    MarkSavesDirty(delta);
  }

  void SyncLanguage() {
    i18n.SetLanguageByCode(store->ResolvedUiLanguage(*system_probe));
  }

  std::optional<fs::path> FindExtractorTool() const {
    std::error_code ec;
    const fs::path script = paths.port_dir / "tools" / "extractor" / "extract_dump.sh";
    if (fs::is_regular_file(script, ec)) {
      return script;
    }
    const fs::path bin = paths.port_dir / "tools" / "extractor" / "pkg_extractor";
    if (fs::is_regular_file(bin, ec)) {
      return bin;
    }
    return std::nullopt;
  }

  void FinalizeGameExit(int exit_code, StateDelta& delta) {
    game_session.reset();
    if (active_mod_overlay) {
      std::error_code ec;
      fs::remove_all(*active_mod_overlay, ec);
      active_mod_overlay.reset();
    }
    game_state.running = false;
    game_state.last_exit_code = exit_code;
    AppendLog("=== Process exited with code " + std::to_string(exit_code) + " ===", delta);
    delta.Mark("is_game_running");
    RefreshTrophiesInternal(delta);
    RefreshSavesInternal(delta);

    if (exit_code != 0 && exit_code != kRestartExitCode &&
        (clock->Now() - game_launch_time) < kQuickCrashThreshold) {
      delta.focus_log_tab = true;
    }
  }
};

LauncherCore::LauncherCore(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LauncherCore::~LauncherCore() {
  if (!impl_) return;
  if (impl_->game_session) {
    impl_->game_session->Terminate();
    impl_->game_session.reset();
  }
  if (impl_->extract_session) {
    impl_->extract_session->Terminate();
    impl_->extract_session.reset();
  }
}

LauncherCore::LauncherCore(LauncherCore&&) noexcept = default;
LauncherCore& LauncherCore::operator=(LauncherCore&&) noexcept = default;

ActionResult LauncherCore::OpenDefault(std::unique_ptr<LauncherCore>& out,
                                       std::optional<fs::path> executable_path) {
  return Open(DiscoverPortPaths(std::move(executable_path)), LauncherAdapters{}, out);
}

ActionResult LauncherCore::Open(PortPaths paths,
                                LauncherAdapters adapters,
                                std::unique_ptr<LauncherCore>& out) {
  out.reset();
  SettingsSchema schema;
  std::string err;
  if (!SettingsSchema::LoadFromFile(paths.SchemaPath(), schema, &err)) {
    return {false, err, {}};
  }

  auto impl = std::make_unique<Impl>();
  impl->paths = std::move(paths);
  impl->process_runner = adapters.process_runner ? std::move(adapters.process_runner)
                                                 : std::make_unique<PosixProcessRunner>();
  impl->system_probe = adapters.system_probe ? std::move(adapters.system_probe)
                                             : std::make_unique<LinuxSystemProbe>(impl->paths);
  impl->clock = adapters.clock ? std::move(adapters.clock)
                               : std::make_unique<SteadyClock>();

  impl->store = std::make_unique<SettingsStore>(std::move(schema), impl->paths);

  LoadReport report;
  impl->store->Load(&report, &err);
  impl->SyncLanguage();

  StateDelta initial_delta;
  for (const auto& w : report.warnings) {
    impl->AppendLog("WARNING: " + w, initial_delta);
  }
  impl->RefreshGameStatusInternal(initial_delta);
  impl->RefreshContentInternal(initial_delta);
  impl->RefreshTrophiesInternal(initial_delta);
  impl->RefreshSavesInternal(initial_delta);

  out.reset(new LauncherCore(std::move(impl)));
  return {true, {}, std::move(initial_delta)};
}

const SettingsSchema& LauncherCore::Schema() const noexcept { return impl_->store->Schema(); }
const SettingsStore& LauncherCore::Settings() const noexcept { return *impl_->store; }

ActionResult LauncherCore::SetSetting(std::string_view key, SettingValue value) {
  std::string err;
  if (!impl_->store->Set(key, std::move(value), &err)) {
    return {false, err, {}};
  }

  StateDelta delta;
  delta.Mark(std::string(key));

  if (!impl_->store->Save(&err)) {
    impl_->AppendLog("ERROR: " + err, delta);
    return {false, err, std::move(delta)};
  }

  if (key == "game_dir") {
    impl_->RefreshGameStatusInternal(delta);
  } else if (key == "user_dir" || key == "language") {
    impl_->RefreshTrophiesInternal(delta);
    impl_->RefreshSavesInternal(delta);
  } else if (key == "mods_dir" || key == "patches_dir") {
    impl_->RefreshContentInternal(delta);
  } else if (key == "ui_language") {
    impl_->SyncLanguage();
    impl_->MarkTranslationsDirty(delta);
  }

  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::SaveSettings() {
  std::string err;
  if (!impl_->store->Save(&err)) {
    return {false, err, {}};
  }
  return {true, {}, {}};
}

LaunchPlan LauncherCore::CompileLaunchPlan() const {
  return LaunchEnvCompiler::Compile(impl_->paths, *impl_->store, *impl_->system_probe);
}

ActionResult LauncherCore::LaunchGame() {
  StateDelta delta;
  if (impl_->game_state.running) {
    return {false, "Game is already running", {}};
  }

  std::string err;
  if (!impl_->store->Save(&err)) {
    impl_->AppendLog("ERROR: Failed to save settings before launch: " + err, delta);
    delta.focus_log_tab = true;
    return {false, err, std::move(delta)};
  }

  impl_->console_log.clear();
  impl_->AppendLog("=== Launching Bloodborne PC ===", delta);

  const LaunchPlan plan = CompileLaunchPlan();
  for (const auto& w : plan.warnings) {
    impl_->AppendLog("WARNING: " + w, delta);
  }

  std::error_code ec;
#ifdef _WIN32
  const char* probe_name = "bb-probe.exe";
#else
  const char* probe_name = "bb-probe";
#endif
  fs::path direct_probe = impl_->paths.port_dir / (impl_->paths.packaged ? "bin" : "out") / probe_name;
  if (!fs::is_regular_file(direct_probe, ec)) {
    direct_probe = impl_->paths.port_dir / "bin" / probe_name;
  }
  if (!fs::is_regular_file(direct_probe, ec)) {
    direct_probe = impl_->paths.port_dir / "out" / probe_name;
  }
  const bool can_launch_natively =
      fs::is_regular_file(direct_probe, ec) &&
      fs::is_regular_file(impl_->paths.BuiltinPatchesXmlPath(), ec);

  SpawnSpec spec;
  spec.inherit_env = false;

  if (can_launch_natively) {
    std::map<std::string, std::string> env = plan.env;
    env["BB_PROBE"] = direct_probe.string();
    env["BB_RESTART_BY_EXIT"] = "1";
    env["BB_GPU_USER_DIR"] = impl_->store->ResolvedSavesDir().string();
    env["__GL_SHADER_DISK_CACHE"] = "1";
    env["__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"] = "1";
    env["__GL_SHADER_DISK_CACHE_SIZE"] = "10737418240";
    env["MESA_SHADER_CACHE_MAX_SIZE"] = "4G";

    NativeExecutionPlan native_plan;
    std::string prep_err;
    const bool skip_prep = (env.count("BB_SKIP_PREPARE") && env["BB_SKIP_PREPARE"] == "1");
    if (!NativeLaunchPlanner::PlanAndPrepare(
            env,
            impl_->paths.port_dir,
            impl_->paths.data_dir,
            {},
            skip_prep,
            false,
            native_plan,
            &prep_err,
            [&](const std::string& line) { impl_->AppendLog(line, delta); })) {
      impl_->AppendLog("ERROR: " + prep_err, delta);
      delta.focus_log_tab = true;
      return {false, prep_err, std::move(delta)};
    }

    impl_->active_mod_overlay = native_plan.mod_game;
    spec.executable = native_plan.probe_executable;
    spec.args = native_plan.probe_args;
    spec.cwd = impl_->paths.data_dir;
    spec.env = native_plan.full_env;
  } else {
    if (!fs::is_regular_file(plan.executable, ec)) {
      const std::string msg = "Runner script not found: " + plan.executable.string();
      impl_->AppendLog("ERROR: " + msg, delta);
      delta.focus_log_tab = true;
      return {false, msg, std::move(delta)};
    }

    if (!impl_->game_inspection.valid) {
      impl_->AppendLog("WARNING: Game Directory does not appear to contain eboot.bin and dvdroot_ps4.", delta);
    }

    spec.cwd = plan.cwd;
    spec.env = plan.env;
#ifdef _WIN32
    if (plan.executable.extension() == ".exe") {
      spec.executable = plan.executable;
      spec.args = {"--play"};
    } else {
      spec.executable = "python.exe";
      spec.args = {plan.executable.string()};
    }
#else
    spec.executable = "/usr/bin/env";
    spec.args = {"bash", plan.executable.string()};
    if (fs::is_regular_file("/bin/bash", ec)) {
      spec.executable = "/bin/bash";
      spec.args = {plan.executable.string()};
    }
#endif
  }

  auto spawned = impl_->process_runner->Spawn(spec);
  if (!spawned) {
    impl_->AppendLog("ERROR: Failed to launch game: " + spawned.error, delta);
    delta.focus_log_tab = true;
    return {false, spawned.error, std::move(delta)};
  }

  impl_->game_session = std::move(spawned.session);
  impl_->game_state.running = true;
  impl_->game_state.last_exit_code.reset();
  impl_->game_launch_time = impl_->clock->Now();
  delta.Mark("is_game_running");
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::StopGame() {
  StateDelta delta;
  if (!impl_->game_state.running || !impl_->game_session) {
    return {true, {}, {}};
  }
  impl_->game_session->Terminate();
  std::vector<std::string> tail;
  impl_->game_session->DrainOutput(tail);
  for (const auto& line : tail) {
    impl_->AppendLog(line, delta);
  }
  const int code = impl_->game_session->TryWait().value_or(143);
  impl_->FinalizeGameExit(code, delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::StartExtraction(const fs::path& dump_path, const fs::path& output_dir) {
  StateDelta delta;
  if (impl_->extraction_state.active) {
    return {false, "An extraction is already running", {}};
  }
  if (dump_path.empty() || output_dir.empty()) {
    return {false, "Dump archive and destination directory are required", {}};
  }

  const auto tool = impl_->FindExtractorTool();
  if (!tool) {
    const std::string msg = "Extractor tool not found under " + (impl_->paths.port_dir / "tools" / "extractor").string();
    impl_->extraction_state.active = false;
    impl_->extraction_state.progress_pct = 0;
    impl_->extraction_state.status_text = msg;
    impl_->AppendLog("ERROR: " + msg, delta);
    impl_->MarkExtractionDirty(delta);
    delta.focus_log_tab = true;
    return {false, msg, std::move(delta)};
  }

  SpawnSpec spec;
  spec.cwd = impl_->paths.port_dir;
  spec.env = impl_->system_probe->HostEnvironment();
  spec.inherit_env = false;

  if (tool->extension() == ".sh") {
    spec.executable = "/bin/bash";
    spec.args = {tool->string(), dump_path.string(), output_dir.string()};
  } else {
    spec.executable = *tool;
    spec.args = {dump_path.string(), output_dir.string()};
  }

  auto spawned = impl_->process_runner->Spawn(spec);
  if (!spawned) {
    impl_->extraction_state.active = false;
    impl_->extraction_state.status_text = "Failed to start extractor: " + spawned.error;
    impl_->AppendLog("ERROR: " + impl_->extraction_state.status_text, delta);
    impl_->MarkExtractionDirty(delta);
    delta.focus_log_tab = true;
    return {false, spawned.error, std::move(delta)};
  }

  impl_->extract_session = std::move(spawned.session);
  impl_->pending_extract_out_dir = output_dir;
  impl_->extraction_state.active = true;
  impl_->extraction_state.progress_pct = 0;
  impl_->extraction_state.status_text = "Extracting " + dump_path.filename().string() + "...";
  impl_->AppendLog("=== Starting extraction: " + dump_path.string() + " -> " + output_dir.string() + " ===", delta);
  impl_->MarkExtractionDirty(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::StartUpdateInstall(const fs::path& update_pkg_path) {
  const auto game_dir = impl_->store->ResolvedGameDir();
  if (game_dir.empty()) {
    return {false, "Select a Game Directory before installing update 1.09", {}};
  }
  return StartExtraction(update_pkg_path, game_dir);
}

ActionResult LauncherCore::ToggleMod(std::string_view mod_id) {
  auto it = std::find_if(impl_->mods.begin(), impl_->mods.end(), [&](const ModItem& m) { return m.id == mod_id; });
  if (it == impl_->mods.end()) {
    return {false, "Mod not found: " + std::string(mod_id), {}};
  }
  it->enabled = !it->enabled;
  if (!SaveModsConfig(impl_->paths.ModsConfigPath(), impl_->mods)) {
    return {false, "Failed to write mods.json", {}};
  }
  StateDelta delta;
  delta.Mark("mods_list");
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::MoveMod(std::string_view mod_id, int delta_index) {
  auto it = std::find_if(impl_->mods.begin(), impl_->mods.end(), [&](const ModItem& m) { return m.id == mod_id; });
  if (it == impl_->mods.end()) {
    return {false, "Mod not found: " + std::string(mod_id), {}};
  }
  const int cur = static_cast<int>(std::distance(impl_->mods.begin(), it));
  const int target = cur + delta_index;
  if (target < 0 || target >= static_cast<int>(impl_->mods.size()) || target == cur) {
    return {true, {}, {}};
  }
  std::swap(impl_->mods[static_cast<std::size_t>(cur)], impl_->mods[static_cast<std::size_t>(target)]);
  for (std::size_t i = 0; i < impl_->mods.size(); ++i) {
    impl_->mods[i].order = static_cast<int>(i);
  }
  if (!SaveModsConfig(impl_->paths.ModsConfigPath(), impl_->mods)) {
    return {false, "Failed to write mods.json", {}};
  }
  StateDelta delta;
  delta.Mark("mods_list");
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::TogglePatch(std::string_view patch_id) {
  auto it = std::find_if(impl_->patches.begin(), impl_->patches.end(),
                         [&](const PatchItem& p) { return p.id == patch_id; });
  if (it == impl_->patches.end()) {
    return {false, "External patch not found: " + std::string(patch_id), {}};
  }
  it->enabled = !it->enabled;
  if (!SaveExternalPatchesConfig(impl_->paths.PatchesConfigPath(), impl_->patches)) {
    return {false, "Failed to write patches.json", {}};
  }
  StateDelta delta;
  delta.Mark("patches_list");
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RefreshGameStatus() {
  StateDelta delta;
  impl_->RefreshGameStatusInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RefreshContent() {
  StateDelta delta;
  impl_->RefreshContentInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RefreshTrophies() {
  StateDelta delta;
  impl_->RefreshTrophiesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RefreshSaves() {
  StateDelta delta;
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::CreateBackup(std::string_view tag) {
  StateDelta delta;
  std::string backup_id;
  std::string err;
  if (!CreateSaveBackup(impl_->store->ResolvedSavesDir(), tag, &backup_id, &err)) {
    impl_->save_feedback = "Backup failed: " + err;
    impl_->AppendLog("ERROR: " + impl_->save_feedback, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  impl_->save_feedback = "Created backup snapshot: " + backup_id;
  impl_->AppendLog("Savedata backup created: " + backup_id, delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RestoreBackup(std::string_view backup_id) {
  StateDelta delta;
  if (impl_->game_state.running) {
    const std::string err = "Cannot restore save backup while Bloodborne is running";
    impl_->save_feedback = err;
    impl_->AppendLog("ERROR: " + err, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  std::string err;
  if (!RestoreSaveBackup(impl_->store->ResolvedSavesDir(), backup_id, &err)) {
    impl_->save_feedback = "Restore failed: " + err;
    impl_->AppendLog("ERROR: " + impl_->save_feedback, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  impl_->save_feedback = "Restored snapshot: " + std::string(backup_id);
  impl_->AppendLog("Restored savedata snapshot: " + std::string(backup_id), delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::DeleteBackup(std::string_view backup_id) {
  StateDelta delta;
  std::string err;
  if (!DeleteSaveBackup(impl_->store->ResolvedSavesDir(), backup_id, &err)) {
    impl_->save_feedback = "Delete failed: " + err;
    impl_->AppendLog("ERROR: " + impl_->save_feedback, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  impl_->save_feedback = "Deleted backup snapshot: " + std::string(backup_id);
  impl_->AppendLog("Deleted savedata snapshot: " + std::string(backup_id), delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RestoreSlotFromInternalBackup(std::string_view slot_id) {
  StateDelta delta;
  if (impl_->game_state.running) {
    const std::string err = "Cannot restore character slot while Bloodborne is running";
    impl_->save_feedback = err;
    impl_->AppendLog("ERROR: " + err, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  std::string err;
  if (!RestoreInternalSlotBackup(impl_->store->ResolvedSavesDir(), slot_id, &err)) {
    impl_->save_feedback = "Slot recovery failed: " + err;
    impl_->AppendLog("ERROR: " + impl_->save_feedback, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  impl_->save_feedback = "Restored slot from internal backup: " + std::string(slot_id);
  impl_->AppendLog(impl_->save_feedback, delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::RepairSavesAudioFlag() {
  StateDelta delta;
  if (impl_->game_state.running) {
    const std::string err = "Cannot patch save audio flag while Bloodborne is running";
    impl_->save_feedback = err;
    impl_->AppendLog("ERROR: " + err, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  const int count = RepairSaveSoundFlags(impl_->store->ResolvedSavesDir());
  impl_->save_feedback = count > 0
                             ? ("Verified & patched 0x204E audio flag in " + std::to_string(count) + " system save file(s)")
                             : "No system save files (userdata0010) found to patch";
  impl_->AppendLog(impl_->save_feedback, delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::ImportSaveFolder(const fs::path& external_dir) {
  StateDelta delta;
  if (impl_->game_state.running) {
    const std::string err = "Cannot import saves while Bloodborne is running";
    impl_->save_feedback = err;
    impl_->AppendLog("ERROR: " + err, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  std::string err;
  const std::string default_tid = impl_->game_inspection.title_id.empty() ? "CUSA03173" : impl_->game_inspection.title_id;
  if (!ImportExternalSaveFolder(impl_->store->ResolvedSavesDir(), external_dir, default_tid, &err)) {
    impl_->save_feedback = "Import failed: " + err;
    impl_->AppendLog("ERROR: " + impl_->save_feedback, delta);
    impl_->MarkSavesDirty(delta);
    return {false, err, std::move(delta)};
  }
  impl_->save_feedback = "Imported save data from: " + external_dir.filename().string();
  impl_->AppendLog("Imported save data from: " + external_dir.string(), delta);
  impl_->RefreshSavesInternal(delta);
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::ClearLog() {
  impl_->console_log.clear();
  StateDelta delta;
  delta.Mark("console_log");
  return {true, {}, std::move(delta)};
}

ActionResult LauncherCore::SaveLog(const fs::path& dest_path) {
  std::ofstream out(dest_path);
  if (!out.is_open()) {
    return {false, "Unable to write log file: " + dest_path.string(), {}};
  }
  out << impl_->console_log;
  StateDelta delta;
  impl_->AppendLog("Log saved to: " + dest_path.string(), delta);
  return {true, {}, std::move(delta)};
}

void LauncherCore::PostSetting(std::string key, SettingValue value) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  impl_->inbox.push_back(PostSetSettingCmd{std::move(key), std::move(value)});
}

void LauncherCore::PostExtraction(fs::path dump_path, fs::path output_dir) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  impl_->inbox.push_back(PostExtractionCmd{std::move(dump_path), std::move(output_dir)});
}

void LauncherCore::PostUpdateInstall(fs::path update_pkg_path) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  impl_->inbox.push_back(PostUpdateInstallCmd{std::move(update_pkg_path)});
}

void LauncherCore::PostSaveLog(fs::path dest_path) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  impl_->inbox.push_back(PostSaveLogCmd{std::move(dest_path)});
}

void LauncherCore::PostImportSaveFolder(fs::path external_dir) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  impl_->inbox.push_back(PostImportSaveFolderCmd{std::move(external_dir)});
}

StateDelta LauncherCore::Poll() {
  StateDelta delta;

  // 1. Drain cross-thread inbox safely on UI thread.
  std::vector<InboxCommand> cmds;
  {
    std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
    cmds.swap(impl_->inbox);
  }
  for (auto& cmd : cmds) {
    if (auto* c = std::get_if<PostSetSettingCmd>(&cmd)) {
      delta.Merge(SetSetting(c->key, std::move(c->value)).delta);
    } else if (auto* c = std::get_if<PostExtractionCmd>(&cmd)) {
      delta.Merge(StartExtraction(c->dump_path, c->output_dir).delta);
    } else if (auto* c = std::get_if<PostUpdateInstallCmd>(&cmd)) {
      delta.Merge(StartUpdateInstall(c->update_pkg_path).delta);
    } else if (auto* c = std::get_if<PostSaveLogCmd>(&cmd)) {
      delta.Merge(SaveLog(c->dest_path).delta);
    } else if (auto* c = std::get_if<PostImportSaveFolderCmd>(&cmd)) {
      delta.Merge(ImportSaveFolder(c->external_dir).delta);
    }
  }

  // 2. Drain running game process output and non-blocking exit.
  if (impl_->game_session) {
    std::vector<std::string> lines;
    impl_->game_session->DrainOutput(lines);
    for (const auto& line : lines) {
      impl_->AppendLog(line, delta);
    }
    if (auto exit_code = impl_->game_session->TryWait(); exit_code.has_value()) {
      std::vector<std::string> tail;
      impl_->game_session->DrainOutput(tail);
      for (const auto& line : tail) {
        impl_->AppendLog(line, delta);
      }
      const int code = *exit_code;
      impl_->FinalizeGameExit(code, delta);
      if (code == kRestartExitCode) {
        impl_->AppendLog("— restarting through native launch planner —", delta);
        delta.Merge(LaunchGame().delta);
      }
    }
  }

  // 3. Drain extraction process output and non-blocking exit.
  if (impl_->extract_session) {
    std::vector<std::string> lines;
    impl_->extract_session->DrainOutput(lines);
    for (const auto& line : lines) {
      impl_->AppendLog(line, delta);
    }
    if (auto exit_code = impl_->extract_session->TryWait(); exit_code.has_value()) {
      std::vector<std::string> tail;
      impl_->extract_session->DrainOutput(tail);
      for (const auto& line : tail) {
        impl_->AppendLog(line, delta);
      }
      impl_->extract_session.reset();
      impl_->extraction_state.active = false;
      if (*exit_code == 0) {
        impl_->extraction_state.progress_pct = 100;
        impl_->extraction_state.status_text = "Extraction complete";
        impl_->AppendLog("=== Extraction completed successfully ===", delta);
        delta.Merge(SetSetting("game_dir", impl_->pending_extract_out_dir.string()).delta);
      } else {
        impl_->extraction_state.progress_pct = 0;
        impl_->extraction_state.status_text = "Extraction failed (exit " + std::to_string(*exit_code) + ")";
        impl_->AppendLog("ERROR: " + impl_->extraction_state.status_text, delta);
        delta.focus_log_tab = true;
      }
      impl_->MarkExtractionDirty(delta);
    }
  }

  return delta;
}

const PortPaths& LauncherCore::Paths() const noexcept { return impl_->paths; }
const GameInspection& LauncherCore::GameStatus() const noexcept { return impl_->game_inspection; }
const std::vector<ModItem>& LauncherCore::Mods() const noexcept { return impl_->mods; }
const std::vector<PatchItem>& LauncherCore::Patches() const noexcept { return impl_->patches; }
const std::vector<TrophyItem>& LauncherCore::Trophies() const noexcept { return impl_->trophy_catalog.trophies; }
const TrophyStats& LauncherCore::TrophySummary() const noexcept { return impl_->trophy_catalog.stats; }

std::string LauncherCore::TrophyProgressText() const {
  const auto& s = impl_->trophy_catalog.stats;
  return std::to_string(s.unlocked) + " / " + std::to_string(s.total) + " (" + std::to_string(s.progress_pct) + "%)";
}

const SaveSummary& LauncherCore::SaveInfo() const noexcept { return impl_->save_summary; }
const std::vector<SaveSlotItem>& LauncherCore::SaveSlots() const noexcept { return impl_->save_slots; }
const std::vector<SaveBackupItem>& LauncherCore::SaveBackups() const noexcept { return impl_->save_backups; }
const std::string& LauncherCore::SaveFeedback() const noexcept { return impl_->save_feedback; }

const ProcessState& LauncherCore::GameProcess() const noexcept { return impl_->game_state; }
const ExtractionState& LauncherCore::Extraction() const noexcept { return impl_->extraction_state; }
const std::string& LauncherCore::ConsoleLog() const noexcept { return impl_->console_log; }
std::string LauncherCore::UiLanguage() const { return impl_->store->ResolvedUiLanguage(*impl_->system_probe); }
const std::string& LauncherCore::Translate(const std::string& key) const { return impl_->i18n.Translate(key); }

} // namespace bbport

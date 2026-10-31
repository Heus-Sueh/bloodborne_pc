#pragma once

#include "config_manager.hpp"
#include "i18n.hpp"
#include "mods_manager.hpp"
#include "trophy_manager.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_Window;

namespace bbport {

class LauncherModel {
public:
  LauncherModel(Rml::Context* context, SDL_Window* window = nullptr);
  ~LauncherModel();

  void Update();
  void Launch();
  bool ShouldClose() const { return should_close_; }

  void ChooseGameDirectory();
  void ChooseUserDirectory();
  void ChooseModsDirectory();
  void ChooseDumpToExtract();
  void StartExtraction(const std::string& dump_path, const std::string& out_dir);
  void ChooseUpdateToInstall();
  void StartUpdateInstallation(const std::string& update_pkg_path);

  void ToggleConsole();
  void ClearLog();
  void CopyLogToClipboard();
  void SaveLogDialog();
  void SaveLogToFile(const std::string& filepath);
  void StopGame();
  void AppendLogLine(const std::string& line);
  void OnGameExited(int exit_code);

  void SetPendingGameDir(const std::string& path);
  void SetPendingUserDir(const std::string& path);
  void SetPendingModsDir(const std::string& path);

  void SetLanguage(const std::string& code);
  void RefreshMods();
  void RefreshTrophies();

  // State exposed to RML template via Data Binding
  LauncherSettings settings;
  std::string active_tab = "play";
  std::string status_text;
  std::string console_log;
  std::string copy_btn_text = "COPY";
  std::string game_version;
  std::string game_title_id;
  bool is_valid = false;
  bool is_update_needed = false;
  bool should_close_ = false;
  bool is_console_open = false;
  bool is_game_running = false;
  bool is_extracting = false;
  std::string extract_progress_pct = "0";
  std::string extract_progress_text;

  // Mods & Patches data
  std::vector<ModItem> mods_list;
  std::vector<PatchItem> patches_list;

  // Trophies data
  std::vector<TrophyItem> trophies_list;
  TrophyStats trophy_stats;
  std::string trophy_progress_text;

  // Translations map exposed for template strings
  std::string tr_tab_play;
  std::string tr_tab_graphics;
  std::string tr_tab_effects;
  std::string tr_tab_controls;
  std::string tr_tab_mods;
  std::string tr_tab_trophies;
  std::string tr_tab_online;
  std::string tr_tab_log;
  std::string tr_launch_game;
  std::string tr_stop_game;

private:
  void BindModel();
  void UpdateValidationStatus();
  void UpdateTranslations();

  Rml::Context* context_ = nullptr;
  SDL_Window* window_ = nullptr;
  Rml::DataModelHandle model_handle_;

  ProcessHandle running_process_;
  std::thread log_reader_thread_;
  std::thread extract_thread_;
  std::mutex log_mutex_;
  std::vector<std::string> pending_log_lines_;

  std::string pending_game_dir_;
  std::string pending_user_dir_;
  std::string pending_mods_dir_;
  std::string pending_dump_to_extract_;
  std::string pending_extract_dest_;
  std::string pending_update_pkg_;
  std::string pending_log_save_path_;

  bool has_pending_game_dir_ = false;
  bool has_pending_user_dir_ = false;
  bool has_pending_mods_dir_ = false;
  bool has_pending_dump_dest_ = false;
  bool has_pending_start_extract_ = false;
  bool has_pending_update_pkg_ = false;
  bool has_pending_log_save_ = false;
};

} // namespace bbport

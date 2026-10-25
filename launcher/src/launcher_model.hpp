#pragma once

#include "config_manager.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct SDL_Window;

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
  void ChooseExtractionDestination(const std::string& dump_path);
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

private:
  Rml::Context* context_ = nullptr;
  SDL_Window* window_ = nullptr;
  Rml::DataModelHandle model_handle_;

  pid_t game_pid_ = -1;
  int copy_feedback_ticks_ = 0;

  std::mutex pending_mutex_;
  std::optional<std::string> pending_game_dir_;
  std::optional<std::string> pending_user_dir_;
  std::optional<std::string> pending_mods_dir_;
  std::optional<bool> pending_game_running_;
  std::optional<bool> pending_is_extracting_;
  std::optional<std::string> pending_extract_progress_pct_;
  std::optional<std::string> pending_extract_progress_text_;
  std::vector<std::string> pending_logs_;

  std::thread log_reader_thread_;
  std::thread extract_thread_;

  void BindModel();
  void UpdateValidationStatus();
};

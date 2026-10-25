#pragma once

#include "config_manager.h"
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
  bool is_valid = false;
  bool should_close_ = false;
  bool is_console_open = false;
  bool is_game_running = false;

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
  std::vector<std::string> pending_logs_;

  std::thread log_reader_thread_;

  void BindModel();
  void UpdateValidationStatus();
};

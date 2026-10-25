#include "launcher_model.h"
#include <RmlUi/Core/Element.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

LauncherModel::LauncherModel(Rml::Context* context, SDL_Window* window)
  : context_(context), window_(window) {
  settings = ConfigManager::Load();
  BindModel();
  UpdateValidationStatus();
}

LauncherModel::~LauncherModel() {
  StopGame();
  if (log_reader_thread_.joinable()) {
    log_reader_thread_.join();
  }
  if (extract_thread_.joinable()) {
    extract_thread_.join();
  }
}

void LauncherModel::BindModel() {
  Rml::DataModelConstructor constructor = context_->CreateDataModel("launcher");
  if (!constructor) {
    std::cerr << "LauncherModel: Failed to create RmlUi data model 'launcher'\n";
    return;
  }

  constructor.Bind("game_dir", &settings.game_dir);
  constructor.Bind("user_dir", &settings.user_dir);
  constructor.Bind("mods_dir", &settings.mods_dir);
  constructor.Bind("output_res", &settings.output_res);
  constructor.Bind("fullscreen", &settings.fullscreen);
  constructor.Bind("borderless", &settings.borderless);
  constructor.Bind("vsync", &settings.vsync);
  constructor.Bind("fps_limit", &settings.fps_limit);
  constructor.Bind("upscaler", &settings.upscaler);
  constructor.Bind("online_pool", &settings.online_pool);
  constructor.Bind("online_server", &settings.online_server);
  constructor.Bind("mouse_look", &settings.mouse_look);
  constructor.Bind("mouse_sensitivity", &settings.mouse_sensitivity);
  constructor.Bind("mouse_invert_y", &settings.mouse_invert_y);
  constructor.Bind("active_tab", &active_tab);
  constructor.Bind("status_text", &status_text);
  constructor.Bind("is_valid", &is_valid);
  constructor.Bind("console_log", &console_log);
  constructor.Bind("is_console_open", &is_console_open);
  constructor.Bind("is_game_running", &is_game_running);
  constructor.Bind("is_extracting", &is_extracting);
  constructor.Bind("extract_progress_pct", &extract_progress_pct);
  constructor.Bind("extract_progress_text", &extract_progress_text);
  constructor.Bind("copy_btn_text", &copy_btn_text);

  constructor.BindEventCallback(
      "play", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { Launch(); });

  constructor.BindEventCallback(
      "toggle_console",
      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ToggleConsole(); });

  constructor.BindEventCallback("clear_log", [this](Rml::DataModelHandle, Rml::Event&,
                                                    const Rml::VariantList&) { ClearLog(); });

  constructor.BindEventCallback(
      "copy_log",
      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { CopyLogToClipboard(); });

  constructor.BindEventCallback("save_log", [this](Rml::DataModelHandle, Rml::Event&,
                                                   const Rml::VariantList&) { SaveLogDialog(); });

  constructor.BindEventCallback("stop_game", [this](Rml::DataModelHandle, Rml::Event&,
                                                    const Rml::VariantList&) { StopGame(); });

  constructor.BindEventCallback(
      "set_tab", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          active_tab = args[0].Get<Rml::String>();
          model_handle_.DirtyVariable("active_tab");
          if (context_) {
            if (auto* root = context_->GetRootElement()) {
              if (auto* content = root->GetElementById("content")) {
                content->SetScrollTop(0.0f);
              }
            }
          }
        }
      });

  constructor.BindEventCallback(
      "set_resolution", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          settings.output_res = args[0].Get<Rml::String>();
          model_handle_.DirtyVariable("output_res");
          ConfigManager::Save(settings);
        }
      });

  constructor.BindEventCallback(
      "set_upscaler", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          settings.upscaler = args[0].Get<Rml::String>();
          model_handle_.DirtyVariable("upscaler");
          ConfigManager::Save(settings);
        }
      });

  constructor.BindEventCallback(
      "set_pool", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          settings.online_pool = args[0].Get<Rml::String>();
          model_handle_.DirtyVariable("online_pool");
          ConfigManager::Save(settings);
        }
      });

  constructor.BindEventCallback("save",
                                [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                  ConfigManager::Save(settings);
                                  UpdateValidationStatus();
                                });

  constructor.BindEventCallback("choose_game_dir",
                                [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                  ChooseGameDirectory();
                                });

  constructor.BindEventCallback("choose_user_dir",
                                [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                  ChooseUserDirectory();
                                });

  constructor.BindEventCallback("choose_mods_dir",
                                [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                  ChooseModsDirectory();
                                });

  constructor.BindEventCallback("extract_dump",
                                [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                  ChooseDumpToExtract();
                                });

  model_handle_ = constructor.GetModelHandle();
}

void LauncherModel::SetPendingGameDir(const std::string& path) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_game_dir_ = path;
}

void LauncherModel::SetPendingUserDir(const std::string& path) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_user_dir_ = path;
}

void LauncherModel::SetPendingModsDir(const std::string& path) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_mods_dir_ = path;
}

static void RunFolderDialogFallback(const char* title,
                                    std::function<void(const std::string&)> on_selected) {
  std::thread([title, on_selected]() {
    std::string cmd = "zenity --file-selection --directory --title=\"";
    cmd += title;
    cmd += "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      char buf[1024];
      if (fgets(buf, sizeof(buf), fp)) {
        std::string path(buf);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
          path.pop_back();
        }
        if (!path.empty()) {
          on_selected(path);
        }
      }
      pclose(fp);
    }
  }).detach();
}

void LauncherModel::ChooseGameDirectory() {
  const char* default_loc = settings.game_dir.empty() ? nullptr : settings.game_dir.c_str();

  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherModel*>(userdata);
        if (!self)
          return;

        if (filelist && *filelist) {
          self->SetPendingGameDir(*filelist);
        } else if (!filelist) {
          RunFolderDialogFallback(
              "Select Bloodborne Game Directory",
              [self](const std::string& path) { self->SetPendingGameDir(path); });
        }
      },
      this, window_, default_loc, false);
}

void LauncherModel::ChooseUserDirectory() {
  const char* default_loc = settings.user_dir.empty() ? nullptr : settings.user_dir.c_str();

  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherModel*>(userdata);
        if (!self)
          return;

        if (filelist && *filelist) {
          self->SetPendingUserDir(*filelist);
        } else if (!filelist) {
          RunFolderDialogFallback("Select Saves & Data Directory", [self](const std::string& path) {
            self->SetPendingUserDir(path);
          });
        }
      },
      this, window_, default_loc, false);
}

void LauncherModel::ChooseModsDirectory() {
  const char* default_loc = settings.mods_dir.empty() ? nullptr : settings.mods_dir.c_str();

  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherModel*>(userdata);
        if (!self)
          return;

        if (filelist && *filelist) {
          self->SetPendingModsDir(*filelist);
        } else if (!filelist) {
          RunFolderDialogFallback(
              "Select Modifications Directory",
              [self](const std::string& path) { self->SetPendingModsDir(path); });
        }
      },
      this, window_, default_loc, false);
}

static void RunFileDialogFallback(const char* title, const char* filter,
                                  std::function<void(const std::string&)> on_selected) {
  std::thread([title, filter, on_selected]() {
    std::string cmd = "zenity --file-selection --title=\"";
    cmd += title;
    cmd += "\" --file-filter=\"";
    cmd += filter;
    cmd += "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      char buf[1024];
      if (fgets(buf, sizeof(buf), fp)) {
        std::string path(buf);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
          path.pop_back();
        }
        if (!path.empty()) {
          on_selected(path);
        }
      }
      pclose(fp);
    }
  }).detach();
}

void LauncherModel::ChooseDumpToExtract() {
  if (is_extracting || is_game_running) {
    return;
  }

  static const SDL_DialogFileFilter filters[] = {{"PS4 Game Dumps (*.pkg, *.iso)", "pkg;iso"},
                                                 {"All Files", "*"}};

  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherModel*>(userdata);
        if (!self)
          return;

        std::string chosen_file;
        if (filelist && *filelist) {
          chosen_file = *filelist;
        }

        if (chosen_file.empty()) {
          RunFileDialogFallback("Select PlayStation 4 Game Dump (PKG or ISO)",
                                "PS4 Dumps (*.pkg *.iso) | *.pkg *.iso",
                                [self](const std::string& path) {
                                  if (!path.empty()) {
                                    self->ChooseExtractionDestination(path);
                                  }
                                });
          return;
        }

        self->ChooseExtractionDestination(chosen_file);
      },
      this, window_, filters, 2, nullptr, false);
}

void LauncherModel::ChooseExtractionDestination(const std::string& dump_path) {
  if (is_extracting || is_game_running)
    return;

  std::string default_dest = settings.game_dir;
  if (default_dest.empty()) {
    default_dest = (fs::current_path() / "games" / "CUSA03173").string();
  }

  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* pair = static_cast<std::pair<LauncherModel*, std::string>*>(userdata);
        if (!pair)
          return;
        LauncherModel* self = pair->first;
        std::string dump = pair->second;
        delete pair;

        std::string chosen_dir;
        if (filelist && *filelist) {
          chosen_dir = *filelist;
        }

        if (chosen_dir.empty()) {
          RunFolderDialogFallback("Select Destination Folder for Game Extraction",
                                  [self, dump](const std::string& path) {
                                    if (!path.empty()) {
                                      self->StartExtraction(dump, path);
                                    }
                                  });
          return;
        }

        self->StartExtraction(dump, chosen_dir);
      },
      new std::pair<LauncherModel*, std::string>(this, dump_path), window_, default_dest.c_str(),
      false);
}

void LauncherModel::StartExtraction(const std::string& dump_path, const std::string& out_dir) {
  if (is_extracting || is_game_running)
    return;

  is_extracting = true;
  extract_progress_pct = "0";
  extract_progress_text = "Preparing extraction...";
  is_console_open = true;
  status_text = "Extracting game dump...";
  if (model_handle_) {
    model_handle_.DirtyVariable("is_extracting");
    model_handle_.DirtyVariable("extract_progress_pct");
    model_handle_.DirtyVariable("extract_progress_text");
    model_handle_.DirtyVariable("is_console_open");
    model_handle_.DirtyVariable("status_text");
  }

  if (extract_thread_.joinable()) {
    extract_thread_.join();
  }

  extract_thread_ = std::thread([this, dump_path, out_dir]() {
    AppendLogLine("[EXTRACT] Starting extraction of: " + dump_path);
    AppendLogLine("[EXTRACT] Target output directory: " + out_dir);

    std::string script = "./tools/extractor/extract_dump.sh";
#ifdef _WIN32
    script = "tools\\extractor\\extract_dump.bat";
#endif
    std::string cmd = script + " \"" + dump_path + "\" \"" + out_dir + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      AppendLogLine("[EXTRACT] Error: Failed to execute extraction script.");
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_is_extracting_ = false;
        pending_extract_progress_text_ = "Failed to launch extraction script.";
      }
      return;
    }

    char buffer[512];
    std::string detected_game_dir = out_dir;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      std::string line(buffer);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }
      if (!line.empty()) {
        AppendLogLine(line);
        const std::string target_marker = "[EXTRACT] TARGET_GAME_DIR=";
        if (line.rfind(target_marker, 0) == 0) {
          detected_game_dir = line.substr(target_marker.length());
        }
        const std::string progress_marker = "[PROGRESS] ";
        if (line.rfind(progress_marker, 0) == 0) {
          std::string rest = line.substr(progress_marker.length());
          size_t sep = rest.find('|');
          if (sep != std::string::npos) {
            std::string pct_str = rest.substr(0, sep);
            std::string detail = rest.substr(sep + 1);
            try {
              float val = std::stof(pct_str);
              if (val < 0.0f)
                val = 0.0f;
              if (val > 100.0f)
                val = 100.0f;
              int ival = static_cast<int>(val);

              std::lock_guard<std::mutex> lock(pending_mutex_);
              pending_extract_progress_pct_ = std::to_string(ival);
              pending_extract_progress_text_ = detail;
            } catch (...) {}
          }
        }
      }
    }
    int exit_code = pclose(pipe);
    AppendLogLine("[EXTRACT] Extraction process finished with exit code: " +
                  std::to_string(exit_code));

    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      pending_is_extracting_ = false;
      pending_extract_progress_pct_ = "100";
      pending_extract_progress_text_ = (exit_code == 0) ? "Extraction completed successfully!"
                                                        : "Extraction encountered errors.";
      if (exit_code == 0) {
        pending_game_dir_ = detected_game_dir;
      }
    }
  });
}

void LauncherModel::UpdateValidationStatus() {
  is_valid = ConfigManager::ValidateGameDirectory(settings.game_dir);
  if (settings.game_dir.empty()) {
    status_text = "No game folder selected. Please set game_dir in Settings.";
  } else if (is_valid) {
    status_text = "Game folder verified (eboot.bin & shaders found). Ready to hunt.";
  } else {
    fs::path p(settings.game_dir);
    if (fs::exists(p / "eboot.bin") || fs::exists(p / "dvdroot_ps4" / "eboot.bin")) {
      status_text =
          "Base assets missing (gxshader.shaderbnd.dcx). Merge base game with 1.09 update.";
    } else {
      status_text = "Invalid game folder (eboot.bin not found).";
    }
  }

  if (model_handle_) {
    model_handle_.DirtyVariable("is_valid");
    model_handle_.DirtyVariable("status_text");
  }
}

void LauncherModel::ToggleConsole() {
  is_console_open = !is_console_open;
  if (model_handle_) {
    model_handle_.DirtyVariable("is_console_open");
  }
}

void LauncherModel::ClearLog() {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  console_log.clear();
  pending_logs_.clear();
  if (model_handle_) {
    model_handle_.DirtyVariable("console_log");
  }
}

void LauncherModel::CopyLogToClipboard() {
  std::string log_content;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    log_content = console_log;
  }
  if (log_content.empty()) {
    log_content = "[No logs recorded yet.]";
  }
  SDL_SetClipboardText(log_content.c_str());
  copy_btn_text = "COPIED!";
  copy_feedback_ticks_ = 120;
  if (model_handle_) {
    model_handle_.DirtyVariable("copy_btn_text");
  }
  AppendLogLine("[INFO] Console log copied to clipboard.");
}

void LauncherModel::SaveLogToFile(const std::string& filepath) {
  std::string content;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    content = console_log;
  }
  std::ofstream out(filepath);
  if (out.is_open()) {
    out << content << "\n";
    out.close();
    AppendLogLine("[INFO] Log successfully saved to: " + filepath);
  } else {
    AppendLogLine("[ERROR] Failed to write log file: " + filepath);
  }
}

void LauncherModel::SaveLogDialog() {
  const SDL_DialogFileFilter filters[] = {
      {"Log files (*.log)", "log"}, {"Text files (*.txt)", "txt"}, {"All files (*)", "*"}};

  SDL_ShowSaveFileDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherModel*>(userdata);
        if (!self)
          return;
        if (filelist && *filelist) {
          self->SaveLogToFile(*filelist);
        } else if (!filelist) {
          // Fallback with zenity
          std::thread([self]() {
            FILE* fp = popen("zenity --file-selection --save --confirm-overwrite "
                             "--filename=\"bloodborne_boot.log\" 2>/dev/null",
                             "r");
            if (fp) {
              char buf[1024];
              if (fgets(buf, sizeof(buf), fp)) {
                std::string path(buf);
                while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
                  path.pop_back();
                }
                if (!path.empty()) {
                  self->SaveLogToFile(path);
                }
              }
              pclose(fp);
            }
          }).detach();
        }
      },
      this, window_, filters, 3, "bloodborne_boot.log");
}

void LauncherModel::StopGame() {
  if (game_pid_ > 0) {
    AppendLogLine("[PROCESS] Terminating game process (PID " + std::to_string(game_pid_) + ")...");
    kill(game_pid_, SIGTERM);
    game_pid_ = -1;
    is_game_running = false;
    if (model_handle_) {
      model_handle_.DirtyVariable("is_game_running");
    }
  }
}

void LauncherModel::AppendLogLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_logs_.push_back(line);
}

void LauncherModel::OnGameExited(int exit_code) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  game_pid_ = -1;
  pending_game_running_ = false;
  std::string msg =
      "[PROCESS] Game process terminated (exit code: " + std::to_string(exit_code) + ")";
  pending_logs_.push_back(std::move(msg));
}

void LauncherModel::Update() {
  bool settings_changed = false;
  bool logs_changed = false;

  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_game_dir_.has_value()) {
      settings.game_dir = *pending_game_dir_;
      pending_game_dir_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("game_dir");
      }
      settings_changed = true;
    }
    if (pending_user_dir_.has_value()) {
      settings.user_dir = *pending_user_dir_;
      pending_user_dir_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("user_dir");
      }
      settings_changed = true;
    }
    if (pending_mods_dir_.has_value()) {
      settings.mods_dir = *pending_mods_dir_;
      pending_mods_dir_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("mods_dir");
      }
      settings_changed = true;
    }
    if (pending_game_running_.has_value()) {
      is_game_running = *pending_game_running_;
      pending_game_running_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("is_game_running");
      }
    }
    if (pending_is_extracting_.has_value()) {
      is_extracting = *pending_is_extracting_;
      pending_is_extracting_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("is_extracting");
      }
    }
    if (pending_extract_progress_pct_.has_value()) {
      extract_progress_pct = *pending_extract_progress_pct_;
      pending_extract_progress_pct_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("extract_progress_pct");
      }
    }
    if (pending_extract_progress_text_.has_value()) {
      extract_progress_text = *pending_extract_progress_text_;
      pending_extract_progress_text_.reset();
      if (model_handle_) {
        model_handle_.DirtyVariable("extract_progress_text");
      }
    }
    if (!pending_logs_.empty()) {
      for (const auto& line : pending_logs_) {
        if (!console_log.empty()) {
          console_log += '\n';
        }
        console_log += line;
      }
      pending_logs_.clear();

      // Cap log buffer size to ~100 KB
      if (console_log.size() > 100000) {
        console_log = console_log.substr(console_log.size() - 80000);
      }
      logs_changed = true;
    }
  }

  if (settings_changed) {
    ConfigManager::Save(settings);
    UpdateValidationStatus();
  }

  if (logs_changed && model_handle_) {
    model_handle_.DirtyVariable("console_log");

    if (context_) {
      if (auto* doc = context_->GetRootElement()) {
        if (auto* body = doc->GetElementById("console-scroll-body")) {
          body->SetScrollTop(body->GetScrollHeight());
        }
      }
    }
  }

  if (copy_feedback_ticks_ > 0) {
    if (--copy_feedback_ticks_ == 0) {
      copy_btn_text = "COPY";
      if (model_handle_) {
        model_handle_.DirtyVariable("copy_btn_text");
      }
    }
  }

  UpdateValidationStatus();
}

void LauncherModel::Launch() {
  if (!is_valid) {
    status_text = "Cannot launch: please set a valid game folder first.";
    if (model_handle_)
      model_handle_.DirtyVariable("status_text");
    return;
  }

  StopGame();

  is_console_open = true;
  is_game_running = true;
  if (model_handle_) {
    model_handle_.DirtyVariable("is_console_open");
    model_handle_.DirtyVariable("is_game_running");
  }

  AppendLogLine("=============================================================="
                "==================");
  AppendLogLine("[LAUNCH] Initializing Bloodborne PC Port via run.sh...");
  AppendLogLine("=============================================================="
                "==================");

  ProcessHandle proc = ConfigManager::LaunchGameAsync(settings);
  if (proc.pid <= 0) {
    AppendLogLine("[ERROR] Failed to start run.sh process.");
    is_game_running = false;
    if (model_handle_)
      model_handle_.DirtyVariable("is_game_running");
    return;
  }

  game_pid_ = proc.pid;

  if (log_reader_thread_.joinable()) {
    log_reader_thread_.join();
  }

  log_reader_thread_ = std::thread([this, fd = proc.read_fd, pid = proc.pid]() {
    FILE* stream = fdopen(fd, "r");
    if (stream) {
      char buf[2048];
      while (fgets(buf, sizeof(buf), stream)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
          line.pop_back();
        }
        AppendLogLine(line);
      }
      fclose(stream);
    } else {
      close(fd);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    OnGameExited(exit_code);
  });
}

#include "launcher_model.hpp"
#include <RmlUi/Core/Element.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace bbport {

LauncherModel::LauncherModel(Rml::Context* context, SDL_Window* window)
  : context_(context), window_(window) {
  settings = ConfigManager::Load();
  I18n::Instance().SetLanguageByCode(settings.ui_language);
  UpdateTranslations();
  BindModel();
  UpdateValidationStatus();
  RefreshMods();
  RefreshTrophies();
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

void LauncherModel::UpdateTranslations() {
  tr_tab_play = tr("tab_play");
  tr_tab_graphics = tr("tab_graphics");
  tr_tab_effects = tr("tab_effects");
  tr_tab_controls = tr("tab_controls");
  tr_tab_mods = tr("tab_mods");
  tr_tab_trophies = tr("tab_trophies");
  tr_tab_online = tr("tab_online");
  tr_tab_log = tr("tab_log");
  tr_launch_game = tr("launch_game");
  tr_stop_game = tr("stop_game");
}

void LauncherModel::SetLanguage(const std::string& code) {
  settings.ui_language = code;
  I18n::Instance().SetLanguageByCode(code);
  UpdateTranslations();
  ConfigManager::Save(settings);
  RefreshTrophies();

  if (model_handle_) {
    model_handle_.DirtyVariable("ui_language");
    model_handle_.DirtyVariable("tr_tab_play");
    model_handle_.DirtyVariable("tr_tab_graphics");
    model_handle_.DirtyVariable("tr_tab_effects");
    model_handle_.DirtyVariable("tr_tab_controls");
    model_handle_.DirtyVariable("tr_tab_mods");
    model_handle_.DirtyVariable("tr_tab_trophies");
    model_handle_.DirtyVariable("tr_tab_online");
    model_handle_.DirtyVariable("tr_tab_log");
    model_handle_.DirtyVariable("tr_launch_game");
    model_handle_.DirtyVariable("tr_stop_game");
  }
  UpdateValidationStatus();
}

void LauncherModel::RefreshMods() {
  ModsManager::Instance().Refresh(settings.mods_dir, settings.patches_dir);
  mods_list = ModsManager::Instance().GetMods();
  patches_list = ModsManager::Instance().GetPatches();
  if (model_handle_) {
    model_handle_.DirtyVariable("mods_list");
    model_handle_.DirtyVariable("patches_list");
  }
}

void LauncherModel::RefreshTrophies() {
  TrophyManager::Instance().Refresh(settings.user_dir, settings.ui_language);
  trophies_list = TrophyManager::Instance().GetTrophies();
  trophy_stats = TrophyManager::Instance().GetStats();

  std::ostringstream ss;
  ss << trophy_stats.unlocked << " / " << trophy_stats.total << " (" << trophy_stats.progress_pct << "%)";
  trophy_progress_text = ss.str();

  if (model_handle_) {
    model_handle_.DirtyVariable("trophies_list");
    model_handle_.DirtyVariable("trophy_progress_pct");
    model_handle_.DirtyVariable("trophy_progress_text");
  }
}

void LauncherModel::BindModel() {
  Rml::DataModelConstructor constructor = context_->CreateDataModel("launcher");
  if (!constructor) {
    std::cerr << "LauncherModel: Failed to create RmlUi data model 'launcher'\n";
    return;
  }

  // Register Structs
  if (auto mod_handle = constructor.RegisterStruct<ModItem>()) {
    mod_handle.RegisterMember("id", &ModItem::id);
    mod_handle.RegisterMember("name", &ModItem::name);
    mod_handle.RegisterMember("author", &ModItem::author);
    mod_handle.RegisterMember("enabled", &ModItem::enabled);
    mod_handle.RegisterMember("order", &ModItem::order);
  }
  constructor.RegisterArray<std::vector<ModItem>>();

  if (auto patch_handle = constructor.RegisterStruct<PatchItem>()) {
    patch_handle.RegisterMember("id", &PatchItem::id);
    patch_handle.RegisterMember("name", &PatchItem::name);
    patch_handle.RegisterMember("author", &PatchItem::author);
    patch_handle.RegisterMember("enabled", &PatchItem::enabled);
  }
  constructor.RegisterArray<std::vector<PatchItem>>();

  if (auto trophy_handle = constructor.RegisterStruct<TrophyItem>()) {
    trophy_handle.RegisterMember("id", &TrophyItem::id);
    trophy_handle.RegisterMember("name", &TrophyItem::name);
    trophy_handle.RegisterMember("detail", &TrophyItem::detail);
    trophy_handle.RegisterMember("grade_str", &TrophyItem::grade_str);
    trophy_handle.RegisterMember("icon_path", &TrophyItem::icon_path);
    trophy_handle.RegisterMember("date_str", &TrophyItem::date_str);
    trophy_handle.RegisterMember("unlocked", &TrophyItem::unlocked);
    trophy_handle.RegisterMember("hidden", &TrophyItem::hidden);
  }
  constructor.RegisterArray<std::vector<TrophyItem>>();

  // Bind settings
  constructor.Bind("game_dir", &settings.game_dir);
  constructor.Bind("user_dir", &settings.user_dir);
  constructor.Bind("mods_dir", &settings.mods_dir);
  constructor.Bind("patches_dir", &settings.patches_dir);
  constructor.Bind("ui_language", &settings.ui_language);

  // Display & Graphics
  constructor.Bind("output_res", &settings.output_res);
  constructor.Bind("fullscreen", &settings.fullscreen);
  constructor.Bind("borderless", &settings.borderless);
  constructor.Bind("vsync", &settings.vsync);
  constructor.Bind("hdr", &settings.hdr);
  constructor.Bind("aspect_ratio", &settings.aspect_ratio);
  constructor.Bind("fps_mode", &settings.fps_mode);
  constructor.Bind("fps_limit", &settings.fps_limit);
  constructor.Bind("upscaler", &settings.upscaler);
  constructor.Bind("preset", &settings.preset);
  constructor.Bind("sharpen", &settings.sharpen);
  constructor.Bind("sharpness", &settings.sharpness);
  constructor.Bind("anti_aliasing", &settings.anti_aliasing);
  constructor.Bind("vbao", &settings.vbao);
  constructor.Bind("show_fps", &settings.show_fps);
  constructor.Bind("object_motion", &settings.object_motion);

  // Effects
  constructor.Bind("motion_blur", &settings.motion_blur);
  constructor.Bind("bloom", &settings.bloom);
  constructor.Bind("chromatic_aberration", &settings.chromatic_aberration);
  constructor.Bind("dof", &settings.dof);
  constructor.Bind("ssao", &settings.ssao);
  constructor.Bind("dynamic_shadows", &settings.dynamic_shadows);
  constructor.Bind("low_latency", &settings.low_latency);

  // Controls
  constructor.Bind("mouse_look", &settings.mouse_look);
  constructor.Bind("mouse_sensitivity", &settings.mouse_sensitivity);
  constructor.Bind("mouse_invert_y", &settings.mouse_invert_y);
  constructor.Bind("mouse_invert_x", &settings.mouse_invert_x);
  constructor.Bind("camera_speed_x", &settings.camera_speed_x);
  constructor.Bind("camera_speed_y", &settings.camera_speed_y);
  constructor.Bind("button_prompts", &settings.button_prompts);

  // Online
  constructor.Bind("online_enabled", &settings.online_enabled);
  constructor.Bind("online_pool", &settings.online_pool);
  constructor.Bind("online_server", &settings.online_server);
  constructor.Bind("discord_token", &settings.discord_token);
  constructor.Bind("local_bbsc", &settings.local_bbsc);

  // Developer
  constructor.Bind("vk_validation", &settings.vk_validation);
  constructor.Bind("save_log", &settings.save_log);

  // UI State
  constructor.Bind("active_tab", &active_tab);
  constructor.Bind("status_text", &status_text);
  constructor.Bind("is_valid", &is_valid);
  constructor.Bind("game_version", &game_version);
  constructor.Bind("game_title_id", &game_title_id);
  constructor.Bind("is_update_needed", &is_update_needed);
  constructor.Bind("console_log", &console_log);
  constructor.Bind("is_console_open", &is_console_open);
  constructor.Bind("is_game_running", &is_game_running);
  constructor.Bind("is_extracting", &is_extracting);
  constructor.Bind("extract_progress_pct", &extract_progress_pct);
  constructor.Bind("extract_progress_text", &extract_progress_text);
  constructor.Bind("copy_btn_text", &copy_btn_text);

  // Lists
  constructor.Bind("mods_list", &mods_list);
  constructor.Bind("patches_list", &patches_list);
  constructor.Bind("trophies_list", &trophies_list);
  constructor.Bind("trophy_progress_pct", &trophy_stats.progress_pct);
  constructor.Bind("trophy_progress_text", &trophy_progress_text);

  // Translations
  constructor.Bind("tr_tab_play", &tr_tab_play);
  constructor.Bind("tr_tab_graphics", &tr_tab_graphics);
  constructor.Bind("tr_tab_effects", &tr_tab_effects);
  constructor.Bind("tr_tab_controls", &tr_tab_controls);
  constructor.Bind("tr_tab_mods", &tr_tab_mods);
  constructor.Bind("tr_tab_trophies", &tr_tab_trophies);
  constructor.Bind("tr_tab_online", &tr_tab_online);
  constructor.Bind("tr_tab_log", &tr_tab_log);
  constructor.Bind("tr_launch_game", &tr_launch_game);
  constructor.Bind("tr_stop_game", &tr_stop_game);

  // Actions & Events
  constructor.BindEventCallback(
      "play", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { Launch(); });

  constructor.BindEventCallback(
      "stop_game", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { StopGame(); });

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
      "set_language", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          SetLanguage(args[0].Get<Rml::String>());
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

  constructor.BindEventCallback(
      "toggle_mod", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          std::string id = args[0].Get<Rml::String>();
          ModsManager::Instance().ToggleMod(id);
          ModsManager::Instance().SaveModsConfig(settings.mods_dir);
          RefreshMods();
        }
      });

  constructor.BindEventCallback(
      "move_mod_up", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          std::string id = args[0].Get<Rml::String>();
          ModsManager::Instance().MoveModUp(id);
          ModsManager::Instance().SaveModsConfig(settings.mods_dir);
          RefreshMods();
        }
      });

  constructor.BindEventCallback(
      "move_mod_down", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          std::string id = args[0].Get<Rml::String>();
          ModsManager::Instance().MoveModDown(id);
          ModsManager::Instance().SaveModsConfig(settings.mods_dir);
          RefreshMods();
        }
      });

  constructor.BindEventCallback(
      "toggle_patch", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
          std::string id = args[0].Get<Rml::String>();
          ModsManager::Instance().TogglePatch(id);
          ModsManager::Instance().SavePatchesConfig(settings.patches_dir);
          RefreshMods();
        }
      });

  constructor.BindEventCallback("save", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ConfigManager::Save(settings);
    UpdateValidationStatus();
  });

  constructor.BindEventCallback("choose_game_dir", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ChooseGameDirectory();
  });

  constructor.BindEventCallback("choose_user_dir", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ChooseUserDirectory();
  });

  constructor.BindEventCallback("choose_mods_dir", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ChooseModsDirectory();
  });

  constructor.BindEventCallback("extract_dump", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ChooseDumpToExtract();
  });

  constructor.BindEventCallback("install_update", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ChooseUpdateToInstall();
  });

  constructor.BindEventCallback("clear_log", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ClearLog();
  });

  constructor.BindEventCallback("copy_log", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    CopyLogToClipboard();
  });

  constructor.BindEventCallback("save_log", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    SaveLogDialog();
  });

  model_handle_ = constructor.GetModelHandle();
}

void LauncherModel::UpdateValidationStatus() {
  is_valid = ConfigManager::ValidateGameDirectory(settings.game_dir);
  game_version = ConfigManager::GetGameVersion(settings.game_dir);
  game_title_id = ConfigManager::GetTitleId(settings.game_dir);

  if (!is_valid) {
    status_text = tr("status_invalid");
    is_update_needed = false;
  } else {
    is_update_needed = !ConfigManager::IsUpdate109(settings.game_dir);
    if (is_update_needed) {
      status_text = tr("status_update_needed");
    } else {
      status_text = tr("status_ready");
    }
  }

  if (model_handle_) {
    model_handle_.DirtyVariable("is_valid");
    model_handle_.DirtyVariable("game_version");
    model_handle_.DirtyVariable("game_title_id");
    model_handle_.DirtyVariable("is_update_needed");
    model_handle_.DirtyVariable("status_text");
  }
}

void LauncherModel::Update() {
  if (has_pending_game_dir_) {
    has_pending_game_dir_ = false;
    settings.game_dir = pending_game_dir_;
    if (model_handle_) model_handle_.DirtyVariable("game_dir");
    ConfigManager::Save(settings);
    UpdateValidationStatus();
  }

  if (has_pending_user_dir_) {
    has_pending_user_dir_ = false;
    settings.user_dir = pending_user_dir_;
    if (model_handle_) model_handle_.DirtyVariable("user_dir");
    ConfigManager::Save(settings);
    RefreshTrophies();
  }

  if (has_pending_mods_dir_) {
    has_pending_mods_dir_ = false;
    settings.mods_dir = pending_mods_dir_;
    if (model_handle_) model_handle_.DirtyVariable("mods_dir");
    ConfigManager::Save(settings);
    RefreshMods();
  }

  if (has_pending_log_save_) {
    has_pending_log_save_ = false;
    SaveLogToFile(pending_log_save_path_);
  }

  // Flush pending log lines
  std::vector<std::string> lines_to_process;
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!pending_log_lines_.empty()) {
      lines_to_process.swap(pending_log_lines_);
    }
  }

  if (!lines_to_process.empty()) {
    for (const auto& line : lines_to_process) {
      console_log += line;
      console_log += "\n";
    }
    if (console_log.size() > 65536) {
      console_log = console_log.substr(console_log.size() - 65536);
    }
    if (model_handle_) {
      model_handle_.DirtyVariable("console_log");
    }
  }

  if (is_game_running && running_process_.IsValid()) {
    int status = 0;
    pid_t result = waitpid(running_process_.pid, &status, WNOHANG);
    if (result > 0) {
      OnGameExited(WEXITSTATUS(status));
    }
  }
}

void LauncherModel::Launch() {
  if (is_game_running) return;
  ConfigManager::Save(settings);

  console_log.clear();
  AppendLogLine("=== Launching Bloodborne PC ===");

  running_process_ = ConfigManager::LaunchGameAsync(settings);
  if (!running_process_.IsValid()) {
    AppendLogLine("ERROR: Failed to launch execution process.");
    return;
  }

  is_game_running = true;
  if (model_handle_) {
    model_handle_.DirtyVariable("is_game_running");
  }

  int read_fd = running_process_.read_fd;
  if (log_reader_thread_.joinable()) {
    log_reader_thread_.join();
  }

  log_reader_thread_ = std::thread([this, read_fd]() {
    char buffer[1024];
    std::string current_line;
    ssize_t bytes_read;
    while ((bytes_read = read(read_fd, buffer, sizeof(buffer) - 1)) > 0) {
      buffer[bytes_read] = '\0';
      for (ssize_t i = 0; i < bytes_read; ++i) {
        if (buffer[i] == '\n') {
          AppendLogLine(current_line);
          current_line.clear();
        } else if (buffer[i] != '\r') {
          current_line += buffer[i];
        }
      }
    }
    if (!current_line.empty()) {
      AppendLogLine(current_line);
    }
    close(read_fd);
  });
}

void LauncherModel::StopGame() {
  if (!is_game_running || !running_process_.IsValid()) return;
  kill(running_process_.pid, SIGTERM);
  int status = 0;
  waitpid(running_process_.pid, &status, 0);
  OnGameExited(WEXITSTATUS(status));
}

void LauncherModel::OnGameExited(int exit_code) {
  is_game_running = false;
  running_process_.pid = -1;
  running_process_.read_fd = -1;
  std::ostringstream ss;
  ss << "=== Process exited with code " << exit_code << " ===";
  AppendLogLine(ss.str());

  if (model_handle_) {
    model_handle_.DirtyVariable("is_game_running");
  }
}

void LauncherModel::AppendLogLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  pending_log_lines_.push_back(line);
}

void LauncherModel::ClearLog() {
  console_log.clear();
  if (model_handle_) {
    model_handle_.DirtyVariable("console_log");
  }
}

void LauncherModel::CopyLogToClipboard() {
  SDL_SetClipboardText(console_log.c_str());
  copy_btn_text = "COPIED!";
  if (model_handle_) {
    model_handle_.DirtyVariable("copy_btn_text");
  }
}

void LauncherModel::SaveLogDialog() {
  SDL_ShowSaveFileDialog(
      [](void* userdata, const char* const* filelist, int filter) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->pending_log_save_path_ = *filelist;
          self->has_pending_log_save_ = true;
        }
      },
      this, window_, nullptr, 0, "bbport_log.txt");
}

void LauncherModel::SaveLogToFile(const std::string& filepath) {
  std::ofstream out(filepath);
  if (out.is_open()) {
    out << console_log;
    AppendLogLine("Log saved to: " + filepath);
  }
}

void LauncherModel::ToggleConsole() {
  is_console_open = !is_console_open;
  if (model_handle_) {
    model_handle_.DirtyVariable("is_console_open");
  }
}

void LauncherModel::ChooseGameDirectory() {
  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->SetPendingGameDir(*filelist);
        }
      },
      this, window_, nullptr, false);
}

void LauncherModel::ChooseUserDirectory() {
  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->SetPendingUserDir(*filelist);
        }
      },
      this, window_, nullptr, false);
}

void LauncherModel::ChooseModsDirectory() {
  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->SetPendingModsDir(*filelist);
        }
      },
      this, window_, nullptr, false);
}

void LauncherModel::ChooseDumpToExtract() {
  const SDL_DialogFileFilter filters[] = {{"PlayStation Dump (*.pkg, *.iso)", "pkg;iso"}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->pending_dump_to_extract_ = *filelist;
          // Prompt for destination
          SDL_ShowOpenFolderDialog(
              [](void* udata, const char* const* flist, int) {
                if (flist && *flist && **flist) {
                  auto* m = static_cast<LauncherModel*>(udata);
                  m->StartExtraction(m->pending_dump_to_extract_, *flist);
                }
              },
              self, self->window_, nullptr, false);
        }
      },
      this, window_, filters, 1, nullptr, false);
}

void LauncherModel::StartExtraction(const std::string& dump_path, const std::string& out_dir) {
  is_extracting = true;
  extract_progress_pct = "0";
  extract_progress_text = "Starting dump extraction...";
  if (model_handle_) {
    model_handle_.DirtyVariable("is_extracting");
    model_handle_.DirtyVariable("extract_progress_pct");
    model_handle_.DirtyVariable("extract_progress_text");
  }

  if (extract_thread_.joinable()) {
    extract_thread_.join();
  }

  extract_thread_ = std::thread([this, dump_path, out_dir]() {
    // Run extraction helper tool
    std::string cmd = "./tools/extractor/extract_dump.sh \"" + dump_path + "\" \"" + out_dir + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
      char buf[256];
      while (fgets(buf, sizeof(buf), pipe)) {
        AppendLogLine(buf);
      }
      pclose(pipe);
    }
    is_extracting = false;
    extract_progress_pct = "100";
    extract_progress_text = "Extraction complete!";
    SetPendingGameDir(out_dir);
    if (model_handle_) {
      model_handle_.DirtyVariable("is_extracting");
      model_handle_.DirtyVariable("extract_progress_pct");
      model_handle_.DirtyVariable("extract_progress_text");
    }
  });
}

void LauncherModel::ChooseUpdateToInstall() {
  const SDL_DialogFileFilter filters[] = {{"Update PKG (*.pkg)", "pkg"}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          auto* self = static_cast<LauncherModel*>(userdata);
          self->StartUpdateInstallation(*filelist);
        }
      },
      this, window_, filters, 1, nullptr, false);
}

void LauncherModel::StartUpdateInstallation(const std::string& update_pkg_path) {
  if (settings.game_dir.empty()) return;
  StartExtraction(update_pkg_path, settings.game_dir);
}

void LauncherModel::SetPendingGameDir(const std::string& path) {
  pending_game_dir_ = path;
  has_pending_game_dir_ = true;
}

void LauncherModel::SetPendingUserDir(const std::string& path) {
  pending_user_dir_ = path;
  has_pending_user_dir_ = true;
}

void LauncherModel::SetPendingModsDir(const std::string& path) {
  pending_mods_dir_ = path;
  has_pending_mods_dir_ = true;
}

} // namespace bbport

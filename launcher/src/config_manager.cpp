#include "config_manager.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

std::vector<std::string> ConfigManager::raw_lines_;

std::string ConfigManager::GetConfigPath() {
  const char* env_config = std::getenv("BB_CONFIG");
  if (env_config && env_config[0]) {
    return env_config;
  }
  const char* env_data = std::getenv("BB_DATA_DIR");
  if (env_data && env_data[0]) {
    fs::path p_toml = fs::path(env_data) / "bbport.toml";
    if (fs::exists(p_toml))
      return p_toml.string();
    fs::path p_ini = fs::path(env_data) / "bbport.ini";
    if (fs::exists(p_ini))
      return p_ini.string();
    return p_toml.string();
  }
  if (fs::exists("bbport.toml")) {
    return "bbport.toml";
  }
  if (fs::exists("bbport.ini")) {
    return "bbport.ini";
  }
  const char* home = std::getenv("HOME");
  if (home) {
    fs::path default_toml = fs::path(home) / ".local" / "share" / "bbport" / "bbport.toml";
    if (fs::exists(default_toml)) {
      return default_toml.string();
    }
    fs::path default_ini = fs::path(home) / ".local" / "share" / "bbport" / "bbport.ini";
    if (fs::exists(default_ini)) {
      return default_ini.string();
    }
  }
  return "bbport.toml";
}

LauncherSettings ConfigManager::Load() {
  LauncherSettings s;
  std::string path = GetConfigPath();
  raw_lines_.clear();

  std::ifstream file(path);
  if (!file.is_open()) {
    return s;
  }

  std::string line;
  while (std::getline(file, line)) {
    raw_lines_.push_back(line);
    std::string trimmed = line;
    size_t comment = trimmed.find('#');
    if (comment != std::string::npos)
      trimmed.erase(comment);

    size_t eq = trimmed.find('=');
    if (eq == std::string::npos)
      continue;

    std::string key = trimmed.substr(0, eq);
    std::string val = trimmed.substr(eq + 1);

    // Trim whitespace
    auto trim_str = [](std::string& str) {
      size_t start = str.find_first_not_of(" \t\r\n");
      size_t end = str.find_last_not_of(" \t\r\n");
      if (start == std::string::npos)
        str = "";
      else
        str = str.substr(start, end - start + 1);
    };
    trim_str(key);
    trim_str(val);

    // Strip quotes for TOML string values
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                            (val.front() == '\'' && val.back() == '\''))) {
      val = val.substr(1, val.size() - 2);
    }

    if (key == "game_dir")
      s.game_dir = val;
    else if (key == "user_dir")
      s.user_dir = val;
    else if (key == "mods_dir")
      s.mods_dir = val;
    else if (key == "output_res")
      s.output_res = val;
    else if (key == "fullscreen")
      s.fullscreen = (val == "1" || val == "true");
    else if (key == "borderless")
      s.borderless = (val == "1" || val == "true");
    else if (key == "vsync")
      s.vsync = (val == "1" || val == "true");
    else if (key == "fps_limit") {
      char* end = nullptr;
      long val_l = std::strtol(val.c_str(), &end, 10);
      if (end != val.c_str())
        s.fps_limit = static_cast<int>(val_l);
    } else if (key == "upscaler")
      s.upscaler = val;
    else if (key == "preset") {
      char* end = nullptr;
      long val_l = std::strtol(val.c_str(), &end, 10);
      if (end != val.c_str())
        s.preset = static_cast<int>(val_l);
    } else if (key == "online_pool")
      s.online_pool = val;
    else if (key == "online_server")
      s.online_server = val;
    else if (key == "mouse_look")
      s.mouse_look = (val == "1" || val == "true");
    else if (key == "mouse_sensitivity")
      s.mouse_sensitivity = std::strtof(val.c_str(), nullptr);
    else if (key == "mouse_invert_y")
      s.mouse_invert_y = (val == "1" || val == "true");
  }

  return s;
}

bool ConfigManager::Save(const LauncherSettings& s) {
  std::string path = GetConfigPath();
  // Migrate legacy .ini to .toml
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".ini") {
    path = path.substr(0, path.size() - 4) + ".toml";
  }

  std::vector<std::string> keys_written;

  auto update_or_append = [&](const std::string& key, const std::string& val) {
    keys_written.push_back(key);
    bool found = false;
    for (auto& line : raw_lines_) {
      std::string trimmed = line;
      size_t comment = trimmed.find('#');
      if (comment != std::string::npos)
        trimmed.erase(comment);
      size_t eq = trimmed.find('=');
      if (eq == std::string::npos)
        continue;
      std::string k = trimmed.substr(0, eq);
      size_t start = k.find_first_not_of(" \t");
      size_t end = k.find_last_not_of(" \t");
      if (start != std::string::npos)
        k = k.substr(start, end - start + 1);
      if (k == key) {
        line = key;
        line += " = ";
        line += val;
        found = true;
        break;
      }
    }
    if (!found) {
      std::string entry = key;
      entry += " = ";
      entry += val;
      raw_lines_.push_back(std::move(entry));
    }
  };

  auto quote = [](const std::string& val) { return "\"" + val + "\""; };

  update_or_append("game_dir", quote(s.game_dir));
  update_or_append("user_dir", quote(s.user_dir));
  update_or_append("mods_dir", quote(s.mods_dir));
  update_or_append("output_res", quote(s.output_res));
  update_or_append("live_resolution", quote("auto"));
  update_or_append("fullscreen", s.fullscreen ? "true" : "false");
  update_or_append("borderless", s.borderless ? "true" : "false");
  update_or_append("vsync", s.vsync ? "true" : "false");
  update_or_append("fps_limit", std::to_string(s.fps_limit));
  update_or_append("upscaler", quote(s.upscaler));
  update_or_append("preset", std::to_string(s.preset));
  update_or_append("online_pool", quote(s.online_pool));
  update_or_append("online_server", quote(s.online_server));
  update_or_append("mouse_look", s.mouse_look ? "true" : "false");
  update_or_append("mouse_sensitivity", std::to_string(s.mouse_sensitivity));
  update_or_append("mouse_invert_y", s.mouse_invert_y ? "true" : "false");

  std::ofstream out(path);
  if (!out.is_open())
    return false;

  for (const auto& l : raw_lines_) {
    out << l << "\n";
  }
  return true;
}

bool ConfigManager::ValidateGameDirectory(const std::string& path) {
  if (path.empty())
    return false;
  fs::path p(path);
  if (!fs::exists(p) || !fs::is_directory(p))
    return false;
  if (!fs::exists(p / "eboot.bin") && !fs::exists(p / "dvdroot_ps4" / "eboot.bin")) {
    return false;
  }
  // Base game assets check: ensure gxshader.shaderbnd.dcx exists
  if (!fs::exists(p / "dvdroot_ps4" / "shader" / "gxshader.shaderbnd.dcx") &&
      !fs::exists(p / "shader" / "gxshader.shaderbnd.dcx")) {
    return false;
  }
  return true;
}

bool ConfigManager::ValidateFsr411(const std::string& directory, const std::string& output,
                                   int preset) {
  int w = 1920;
  int h = 1080;
  size_t x_pos = output.find('x');
  if (x_pos != std::string::npos) {
    char* end = nullptr;
    long parsed_w = std::strtol(output.c_str(), &end, 10);
    long parsed_h = std::strtol(output.c_str() + x_pos + 1, nullptr, 10);
    if (end != output.c_str()) {
      w = static_cast<int>(parsed_w);
      h = static_cast<int>(parsed_h);
    }
  }
  std::string model =
      ((w > 1920 || h > 1080) ? "t2160" : "t1080") + std::string(preset == 4 ? "_m1" : "_m0");
  fs::path base = fs::path(directory) / model;

  std::vector<std::string> passes = {"spd", "prepass", "pass0_post"};
  for (int i = 1; i <= 12; ++i) {
    passes.push_back("pass" + std::to_string(i));
    passes.push_back("pass" + std::to_string(i) + "_post");
  }
  passes.push_back("postpass");
  passes.push_back("rcas");

  for (const auto& name : passes) {
    fs::path p = base / (name + ".spv");
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz < 20 || sz % 4 != 0)
      return false;
  }
  fs::path init_p = base / "initializer.bin";
  std::error_code ec;
  auto sz = fs::file_size(init_p, ec);
  if (ec || sz != 131072)
    return false;
  return true;
}

ProcessHandle ConfigManager::LaunchGameAsync(const LauncherSettings& s) {
  Save(s);

  if (s.game_dir.empty() || !ValidateGameDirectory(s.game_dir)) {
    std::cerr << "bbport: please select a valid game folder containing eboot.bin\n";
    return {-1, -1};
  }

  if (s.online_pool != "offline") {
    setenv("BB_ONLINE", "1", 1);
    if (s.online_pool == "huntersdream") {
      setenv("BB_NET_BACKEND", "thd", 1);
    } else {
      unsetenv("BB_NET_BACKEND");
    }
  } else {
    setenv("BB_ONLINE", "0", 1);
    unsetenv("BB_NET_BACKEND");
  }

  setenv("BB_GAME_DIR", s.game_dir.c_str(), 1);
  setenv("BB_USER_DIR", s.user_dir.c_str(), 1);
  setenv("BB_MODS_DIR", s.mods_dir.c_str(), 1);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    std::perror("pipe failed");
    return {-1, -1};
  }

  UniqueFd read_end(pipe_fds[0]);
  UniqueFd write_end(pipe_fds[1]);

  pid_t pid = fork();
  if (pid == 0) {
    // Child: redirect stdout & stderr to pipe
    read_end.Reset();
    dup2(write_end.Get(), STDOUT_FILENO);
    dup2(write_end.Get(), STDERR_FILENO);
    write_end.Reset();

    if (std::filesystem::exists("out/bb-runner")) {
      execlp("./out/bb-runner", "./out/bb-runner", (char*)nullptr);
    } else if (std::filesystem::exists("scripts/run.sh")) {
      execlp("bash", "bash", "scripts/run.sh", (char*)nullptr);
    } else {
      execlp("bash", "bash", "run.sh", (char*)nullptr);
    }
    std::perror("execlp native runner failed");
    _exit(127);
  } else if (pid > 0) {
    write_end.Reset();
    return {pid, read_end.Release()};
  }

  return {-1, -1};
}

int ConfigManager::LaunchGame(const LauncherSettings& s) {
  ProcessHandle proc = LaunchGameAsync(s);
  if (proc.pid <= 0) {
    return 1;
  }
  close(proc.read_fd);
  int status = 0;
  waitpid(proc.pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

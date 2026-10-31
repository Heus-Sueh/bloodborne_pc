#include "config_manager.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

namespace bbport {

std::vector<std::string> ConfigManager::raw_ini_lines_;

std::string ConfigManager::GetSettingsJsonPath() {
  const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
  if (xdg_config && xdg_config[0]) {
    fs::path p = fs::path(xdg_config) / "bbport-launcher" / "settings.json";
    return p.string();
  }
  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    fs::path p = fs::path(home) / ".config" / "bbport-launcher" / "settings.json";
    return p.string();
  }
  return "settings.json";
}

std::string ConfigManager::GetIniPath() {
  const char* env_config = std::getenv("BB_CONFIG");
  if (env_config && env_config[0]) {
    return env_config;
  }
  if (fs::exists("bbport.ini")) {
    return "bbport.ini";
  }
  const char* env_data = std::getenv("BB_DATA_DIR");
  if (env_data && env_data[0]) {
    fs::path p = fs::path(env_data) / "bbport.ini";
    return p.string();
  }
  return "bbport.ini";
}

LauncherSettings ConfigManager::Load() {
  LauncherSettings s;
  LoadSettingsJson(s);
  LoadIni(s);
  return s;
}

void ConfigManager::LoadSettingsJson(LauncherSettings& s) {
  std::string path = GetSettingsJsonPath();
  if (!fs::exists(path)) return;

  std::ifstream file(path);
  if (!file.is_open()) return;

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  auto get_string = [&](const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return;
    size_t colon = content.find(':', pos + needle.length());
    if (colon == std::string::npos) return;
    size_t q1 = content.find('"', colon);
    if (q1 == std::string::npos) return;
    size_t q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos) return;
    out = content.substr(q1 + 1, q2 - q1 - 1);
  };

  auto get_bool = [&](const std::string& key, bool& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return;
    size_t colon = content.find(':', pos + needle.length());
    if (colon == std::string::npos) return;
    size_t comma = content.find_first_of(",}\n", colon);
    std::string val = content.substr(colon + 1, comma - colon - 1);
    if (val.find("true") != std::string::npos) out = true;
    else if (val.find("false") != std::string::npos) out = false;
  };

  auto get_int = [&](const std::string& key, int& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return;
    size_t colon = content.find(':', pos + needle.length());
    if (colon == std::string::npos) return;
    size_t comma = content.find_first_of(",}\n", colon);
    std::string val = content.substr(colon + 1, comma - colon - 1);
    try { out = std::stoi(val); } catch (...) {}
  };

  get_string("game_dir", s.game_dir);
  get_string("user_dir", s.user_dir);
  get_string("mods_dir", s.mods_dir);
  get_string("patches_dir", s.patches_dir);
  get_string("ui_language", s.ui_language);
  get_bool("fullscreen", s.fullscreen);
  get_bool("hdr", s.hdr);
  get_string("fps_mode", s.fps_mode);
  get_int("fps_limit", s.fps_limit);
  get_bool("online", s.online_enabled);
  get_string("online_pool", s.online_pool);
  get_string("online_server", s.online_server);
  get_string("discord_token", s.discord_token);
  get_bool("online_bbsc", s.local_bbsc);
  get_bool("vk_validation", s.vk_validation);
  get_bool("save_log", s.save_log);
}

void ConfigManager::SaveSettingsJson(const LauncherSettings& s) {
  std::string path = GetSettingsJsonPath();
  try {
    fs::create_directories(fs::path(path).parent_path());
  } catch (...) {}

  std::ofstream file(path);
  if (!file.is_open()) return;

  file << "{\n"
       << "  \"game_dir\": \"" << s.game_dir << "\",\n"
       << "  \"user_dir\": \"" << s.user_dir << "\",\n"
       << "  \"mods_dir\": \"" << s.mods_dir << "\",\n"
       << "  \"patches_dir\": \"" << s.patches_dir << "\",\n"
       << "  \"ui_language\": \"" << s.ui_language << "\",\n"
       << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n"
       << "  \"hdr\": " << (s.hdr ? "true" : "false") << ",\n"
       << "  \"fps_mode\": \"" << s.fps_mode << "\",\n"
       << "  \"fps_limit\": " << s.fps_limit << ",\n"
       << "  \"online\": " << (s.online_enabled ? "true" : "false") << ",\n"
       << "  \"online_pool\": \"" << s.online_pool << "\",\n"
       << "  \"online_server\": \"" << s.online_server << "\",\n"
       << "  \"discord_token\": \"" << s.discord_token << "\",\n"
       << "  \"online_bbsc\": " << (s.local_bbsc ? "true" : "false") << ",\n"
       << "  \"vk_validation\": " << (s.vk_validation ? "true" : "false") << ",\n"
       << "  \"save_log\": " << (s.save_log ? "true" : "false") << "\n"
       << "}\n";
}

void ConfigManager::LoadIni(LauncherSettings& s) {
  std::string path = GetIniPath();
  raw_ini_lines_.clear();
  if (!fs::exists(path)) return;

  std::ifstream file(path);
  if (!file.is_open()) return;

  std::string line;
  while (std::getline(file, line)) {
    raw_ini_lines_.push_back(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    // Trim
    auto trim = [](std::string& str) {
      size_t f = str.find_first_not_of(" \t\r\n");
      if (f == std::string::npos) { str.clear(); return; }
      size_t l = str.find_last_not_of(" \t\r\n");
      str = str.substr(f, l - f + 1);
    };
    trim(key);
    trim(val);

    if (key == "output_res") s.output_res = val;
    else if (key == "aspect_ratio") s.aspect_ratio = val;
    else if (key == "upscaler") s.upscaler = val;
    else if (key == "preset") { try { s.preset = std::stoi(val); } catch (...) {} }
    else if (key == "sharpen") s.sharpen = (val == "1" || val == "true");
    else if (key == "sharpness") { try { s.sharpness = std::stof(val); } catch (...) {} }
    else if (key == "anti_aliasing") s.anti_aliasing = val;
    else if (key == "vbao") s.vbao = (val == "1" || val == "true");
    else if (key == "show_fps") s.show_fps = (val == "1" || val == "true");
    else if (key == "object_motion") s.object_motion = (val == "1" || val == "true");
    else if (key == "effect_motion_blur") s.motion_blur = (val == "1" || val == "true");
    else if (key == "effect_bloom") s.bloom = (val == "1" || val == "true");
    else if (key == "effect_chromatic_aberration") s.chromatic_aberration = (val == "1" || val == "true");
    else if (key == "effect_dof") s.dof = (val == "1" || val == "true");
    else if (key == "effect_ssao") s.ssao = (val == "1" || val == "true");
    else if (key == "effect_dynamic_shadows") s.dynamic_shadows = (val == "1" || val == "true");
    else if (key == "low_latency") s.low_latency = val;
    else if (key == "mouse_look") s.mouse_look = (val == "1" || val == "true");
    else if (key == "mouse_sensitivity") { try { s.mouse_sensitivity = std::stof(val); } catch (...) {} }
    else if (key == "mouse_invert_y") s.mouse_invert_y = (val == "1" || val == "true");
    else if (key == "mouse_invert_x") s.mouse_invert_x = (val == "1" || val == "true");
    else if (key == "camera_speed_x") { try { s.camera_speed_x = std::stof(val); } catch (...) {} }
    else if (key == "camera_speed_y") { try { s.camera_speed_y = std::stof(val); } catch (...) {} }
    else if (key == "button_prompts") s.button_prompts = val;
  }
}

void ConfigManager::SaveIni(const LauncherSettings& s) {
  std::string path = GetIniPath();
  std::unordered_map<std::string, std::string> values = {
    {"output_res", s.output_res},
    {"aspect_ratio", s.aspect_ratio},
    {"upscaler", s.upscaler},
    {"preset", std::to_string(s.preset)},
    {"sharpen", s.sharpen ? "1" : "0"},
    {"sharpness", std::to_string(s.sharpness)},
    {"anti_aliasing", s.anti_aliasing},
    {"vbao", s.vbao ? "1" : "0"},
    {"show_fps", s.show_fps ? "1" : "0"},
    {"object_motion", s.object_motion ? "1" : "0"},
    {"effect_motion_blur", s.motion_blur ? "1" : "0"},
    {"effect_bloom", s.bloom ? "1" : "0"},
    {"effect_chromatic_aberration", s.chromatic_aberration ? "1" : "0"},
    {"effect_dof", s.dof ? "1" : "0"},
    {"effect_ssao", s.ssao ? "1" : "0"},
    {"effect_dynamic_shadows", s.dynamic_shadows ? "1" : "0"},
    {"low_latency", s.low_latency},
    {"mouse_look", s.mouse_look ? "1" : "0"},
    {"mouse_sensitivity", std::to_string(s.mouse_sensitivity)},
    {"mouse_invert_y", s.mouse_invert_y ? "1" : "0"},
    {"mouse_invert_x", s.mouse_invert_x ? "1" : "0"},
    {"camera_speed_x", std::to_string(s.camera_speed_x)},
    {"camera_speed_y", std::to_string(s.camera_speed_y)},
    {"button_prompts", s.button_prompts}
  };

  std::vector<std::string> out;
  std::unordered_map<std::string, bool> written;

  for (const auto& line : raw_ini_lines_) {
    if (!line.empty() && line[0] != '#' && line[0] != ';' && line.find('=') != std::string::npos) {
      std::string k = line.substr(0, line.find('='));
      // trim k
      size_t f = k.find_first_not_of(" \t");
      size_t l = k.find_last_not_of(" \t");
      if (f != std::string::npos) k = k.substr(f, l - f + 1);

      if (values.count(k)) {
        out.push_back(k + "=" + values[k]);
        written[k] = true;
        continue;
      }
    }
    out.push_back(line);
  }

  if (out.empty()) {
    out.push_back("# bbport settings (in-game menu: Insert / hold L3+R3)");
  }

  for (const auto& [k, v] : values) {
    if (!written[k]) {
      out.push_back(k + "=" + v);
    }
  }

  std::ofstream file(path);
  if (!file.is_open()) return;
  for (const auto& l : out) {
    file << l << "\n";
  }
}

bool ConfigManager::Save(const LauncherSettings& settings) {
  SaveSettingsJson(settings);
  SaveIni(settings);
  return true;
}

bool ConfigManager::ValidateGameDirectory(const std::string& path) {
  if (path.empty() || !fs::exists(path) || !fs::is_directory(path)) {
    return false;
  }
  fs::path eboot = fs::path(path) / "eboot.bin";
  fs::path dvdroot = fs::path(path) / "dvdroot_ps4";
  return fs::exists(eboot) && fs::exists(dvdroot);
}

std::string ConfigManager::GetGameVersion(const std::string& path) {
  if (!ValidateGameDirectory(path)) return "Unknown";
  fs::path param_sfo = fs::path(path) / "sce_sys" / "param.sfo";
  if (fs::exists(param_sfo)) {
    std::ifstream f(param_sfo, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = bytes.find("01.");
    if (pos != std::string::npos && pos + 5 <= bytes.size()) {
      return bytes.substr(pos, 5);
    }
  }
  return "01.09";
}

std::string ConfigManager::GetTitleId(const std::string& path) {
  if (!ValidateGameDirectory(path)) return "CUSA03173";
  fs::path param_sfo = fs::path(path) / "sce_sys" / "param.sfo";
  if (fs::exists(param_sfo)) {
    std::ifstream f(param_sfo, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = bytes.find("CUSA");
    if (pos != std::string::npos && pos + 9 <= bytes.size()) {
      return bytes.substr(pos, 9);
    }
  }
  return "CUSA03173";
}

bool ConfigManager::IsUpdate109(const std::string& path) {
  std::string v = GetGameVersion(path);
  return v == "01.09" || v.find("1.09") != std::string::npos;
}

int ConfigManager::LaunchGame(const LauncherSettings& settings) {
  ProcessHandle proc = LaunchGameAsync(settings);
  if (!proc.IsValid()) return -1;
  int status = 0;
  waitpid(proc.pid, &status, 0);
  return WEXITSTATUS(status);
}

ProcessHandle ConfigManager::LaunchGameAsync(const LauncherSettings& settings) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return {};
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return {};
  }

  if (pid == 0) {
    // Child process
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    if (!settings.game_dir.empty()) {
      setenv("BB_GAME_DIR", settings.game_dir.c_str(), 1);
    }
    if (!settings.user_dir.empty()) {
      setenv("BB_USER_DIR", settings.user_dir.c_str(), 1);
    }
    if (!settings.mods_dir.empty()) {
      setenv("BB_MODS_DIR", settings.mods_dir.c_str(), 1);
    }

    // Run execution entry point
    if (fs::exists("./scripts/run.sh")) {
      execl("./scripts/run.sh", "./scripts/run.sh", nullptr);
    } else if (fs::exists("./run.sh")) {
      execl("./run.sh", "./run.sh", nullptr);
    } else {
      execl("./build.sh", "./build.sh", nullptr);
    }
    _exit(127);
  }

  close(pipefd[1]);
  return ProcessHandle{pid, pipefd[0]};
}

} // namespace bbport

#include "config_manager.hpp"
#include <algorithm>
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

void ConfigManager::ValidateSettings(LauncherSettings& s) {
  // Clamping
  if (s.sharpness < 0.0f) s.sharpness = 0.0f;
  if (s.sharpness > 1.0f) s.sharpness = 1.0f;

  if (s.mouse_sensitivity < 0.1f) s.mouse_sensitivity = 0.1f;
  if (s.mouse_sensitivity > 10.0f) s.mouse_sensitivity = 10.0f;

  if (s.camera_speed_x < 0.1f) s.camera_speed_x = 0.1f;
  if (s.camera_speed_x > 10.0f) s.camera_speed_x = 10.0f;

  if (s.camera_speed_y < 0.1f) s.camera_speed_y = 0.1f;
  if (s.camera_speed_y > 10.0f) s.camera_speed_y = 10.0f;

  if (s.fps_limit < 0) s.fps_limit = 0;
  if (s.preset < 0 || s.preset > 4) s.preset = 4;

  if (s.ui_language.empty()) s.ui_language = "en";
  if (s.present_mode.empty()) s.present_mode = "Mailbox";
  if (s.output_res.empty()) s.output_res = "auto";
  if (s.aspect_ratio.empty()) s.aspect_ratio = "auto";
  if (s.upscaler.empty()) s.upscaler = "fsr4";
  if (s.anti_aliasing.empty()) s.anti_aliasing = "auto";
  if (s.texture_quality.empty()) s.texture_quality = "auto";
  if (s.low_latency.empty()) s.low_latency = "on";
  if (s.button_prompts.empty()) s.button_prompts = "auto";
  if (s.online_pool.empty()) s.online_pool = "seamless";
  if (s.fps_mode.empty()) s.fps_mode = "uncap";
}

LauncherSettings ConfigManager::Load() {
  LauncherSettings s;
  LoadSettingsJson(s);
  LoadIni(s);
  ValidateSettings(s);
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

  // Section: Game
  get_string("ui_language", s.ui_language);
  get_string("game_dir", s.game_dir);
  get_string("user_dir", s.user_dir);
  get_string("mods_dir", s.mods_dir);
  get_string("patches_dir", s.patches_dir);
  get_int("language", s.language);
  get_bool("skip_network_choice", s.skip_network_choice);

  // Section: Display
  get_string("display", s.display);
  get_bool("fullscreen", s.fullscreen);
  get_bool("borderless", s.borderless);
  get_string("present_mode", s.present_mode);
  get_bool("hdr", s.hdr);

  // Section: Performance
  get_string("fps_mode", s.fps_mode);
  get_int("fps_limit", s.fps_limit);
  get_string("draw_pipe", s.draw_pipe);
  get_string("gpu", s.gpu);

  // Section: Controls
  get_string("gamepad", s.gamepad);

  // Section: Mods
  get_bool("mods_enabled", s.mods_enabled);

  // Section: Online
  get_bool("online", s.online_enabled);
  get_string("online_pool", s.online_pool);
  get_string("online_community", s.online_community);
  get_string("online_server", s.online_server);
  get_string("online_webapi", s.online_webapi);
  get_bool("online_bbsc", s.local_bbsc);
  get_string("online_npid", s.online_npid);
  get_string("online_password", s.online_password);
  get_string("thd_server", s.thd_server);
  get_string("discord_token", s.discord_token);
  get_bool("online_upnp", s.online_upnp);

  // Section: Developer
  get_bool("save_log", s.save_log);
  get_bool("frame_stats", s.frame_stats);
  get_bool("gpu_profile", s.gpu_profile);
  get_bool("crash_diag", s.crash_diag);
  get_bool("vk_validation", s.vk_validation);
  get_bool("mangohud", s.mangohud);
  get_string("extra_env", s.extra_env);
}

void ConfigManager::SaveSettingsJson(const LauncherSettings& s) {
  std::string path = GetSettingsJsonPath();
  try {
    fs::create_directories(fs::path(path).parent_path());
  } catch (...) {}

  std::ofstream file(path);
  if (!file.is_open()) return;

  file << "{\n"
       << "  \"ui_language\": \"" << s.ui_language << "\",\n"
       << "  \"game_dir\": \"" << s.game_dir << "\",\n"
       << "  \"user_dir\": \"" << s.user_dir << "\",\n"
       << "  \"mods_dir\": \"" << s.mods_dir << "\",\n"
       << "  \"patches_dir\": \"" << s.patches_dir << "\",\n"
       << "  \"language\": " << s.language << ",\n"
       << "  \"skip_network_choice\": " << (s.skip_network_choice ? "true" : "false") << ",\n"
       << "  \"display\": \"" << s.display << "\",\n"
       << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n"
       << "  \"borderless\": " << (s.borderless ? "true" : "false") << ",\n"
       << "  \"present_mode\": \"" << s.present_mode << "\",\n"
       << "  \"hdr\": " << (s.hdr ? "true" : "false") << ",\n"
       << "  \"fps_mode\": \"" << s.fps_mode << "\",\n"
       << "  \"fps_limit\": " << s.fps_limit << ",\n"
       << "  \"draw_pipe\": \"" << s.draw_pipe << "\",\n"
       << "  \"gpu\": \"" << s.gpu << "\",\n"
       << "  \"gamepad\": \"" << s.gamepad << "\",\n"
       << "  \"mods_enabled\": " << (s.mods_enabled ? "true" : "false") << ",\n"
       << "  \"online\": " << (s.online_enabled ? "true" : "false") << ",\n"
       << "  \"online_pool\": \"" << s.online_pool << "\",\n"
       << "  \"online_community\": \"" << s.online_community << "\",\n"
       << "  \"online_server\": \"" << s.online_server << "\",\n"
       << "  \"online_webapi\": \"" << s.online_webapi << "\",\n"
       << "  \"online_bbsc\": " << (s.local_bbsc ? "true" : "false") << ",\n"
       << "  \"online_npid\": \"" << s.online_npid << "\",\n"
       << "  \"online_password\": \"" << s.online_password << "\",\n"
       << "  \"thd_server\": \"" << s.thd_server << "\",\n"
       << "  \"discord_token\": \"" << s.discord_token << "\",\n"
       << "  \"online_upnp\": " << (s.online_upnp ? "true" : "false") << ",\n"
       << "  \"save_log\": " << (s.save_log ? "true" : "false") << ",\n"
       << "  \"frame_stats\": " << (s.frame_stats ? "true" : "false") << ",\n"
       << "  \"gpu_profile\": " << (s.gpu_profile ? "true" : "false") << ",\n"
       << "  \"crash_diag\": " << (s.crash_diag ? "true" : "false") << ",\n"
       << "  \"vk_validation\": " << (s.vk_validation ? "true" : "false") << ",\n"
       << "  \"mangohud\": " << (s.mangohud ? "true" : "false") << ",\n"
       << "  \"extra_env\": \"" << s.extra_env << "\"\n"
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
    auto trim = [](std::string& str) {
      size_t f = str.find_first_not_of(" \t\r\n");
      if (f == std::string::npos) { str.clear(); return; }
      size_t l = str.find_last_not_of(" \t\r\n");
      str = str.substr(f, l - f + 1);
    };
    trim(key);
    trim(val);

    // Section: Game
    if (key == "game_text_language") s.game_text_language = val;
    else if (key == "game_voice_language") s.game_voice_language = val;

    // Section: Display
    else if (key == "output_res") s.output_res = val;
    else if (key == "aspect_ratio") s.aspect_ratio = val;

    // Section: Graphics
    else if (key == "upscaler") s.upscaler = val;
    else if (key == "preset") { try { s.preset = std::stoi(val); } catch (...) {} }
    else if (key == "frame_generation") s.frame_generation = val;
    else if (key == "sharpen") s.sharpen = (val == "1" || val == "true");
    else if (key == "sharpness") { try { s.sharpness = std::stof(val); } catch (...) {} }
    else if (key == "anti_aliasing") s.anti_aliasing = val;
    else if (key == "texture_quality") s.texture_quality = val;
    else if (key == "model_lod") { try { s.model_lod = std::stoi(val); } catch (...) {} }
    else if (key == "object_motion") s.object_motion = (val == "1" || val == "true");
    else if (key == "vbao") s.vbao = (val == "1" || val == "true");
    else if (key == "show_fps") s.show_fps = (val == "1" || val == "true");

    // Section: Effects
    else if (key == "effect_chromatic_aberration") s.chromatic_aberration = (val == "1" || val == "true");
    else if (key == "effect_dof") s.dof = (val == "1" || val == "true");
    else if (key == "effect_motion_blur") s.motion_blur = (val == "1" || val == "true");
    else if (key == "effect_bloom") s.bloom = (val == "1" || val == "true");
    else if (key == "effect_ssao") s.ssao = (val == "1" || val == "true");
    else if (key == "effect_dynamic_shadows") s.dynamic_shadows = (val == "1" || val == "true");
    else if (key == "effect_ssr") s.effect_ssr = (val == "1" || val == "true");
    else if (key == "skip_intro") s.skip_intro = (val == "1" || val == "true");
    else if (key == "debug_camera") s.debug_camera = (val == "1" || val == "true");
    else if (key == "debug_menu") s.debug_menu = (val == "1" || val == "true");

    // Section: Performance
    else if (key == "low_latency") s.low_latency = val;

    // Section: Controls
    else if (key == "button_prompts") s.button_prompts = val;
    else if (key == "mouse_look") s.mouse_look = (val == "1" || val == "true");
    else if (key == "mouse_sensitivity") { try { s.mouse_sensitivity = std::stof(val); } catch (...) {} }
    else if (key == "mouse_invert_y") s.mouse_invert_y = (val == "1" || val == "true");
    else if (key == "mouse_invert_x") s.mouse_invert_x = (val == "1" || val == "true");
    else if (key == "camera_speed_x") { try { s.camera_speed_x = std::stof(val); } catch (...) {} }
    else if (key == "camera_speed_y") { try { s.camera_speed_y = std::stof(val); } catch (...) {} }
  }
}

void ConfigManager::SaveIni(const LauncherSettings& s) {
  std::string path = GetIniPath();
  std::unordered_map<std::string, std::string> values = {
    {"game_text_language", s.game_text_language},
    {"game_voice_language", s.game_voice_language},
    {"output_res", s.output_res},
    {"aspect_ratio", s.aspect_ratio},
    {"upscaler", s.upscaler},
    {"preset", std::to_string(s.preset)},
    {"frame_generation", s.frame_generation},
    {"sharpen", s.sharpen ? "1" : "0"},
    {"sharpness", std::to_string(s.sharpness)},
    {"anti_aliasing", s.anti_aliasing},
    {"texture_quality", s.texture_quality},
    {"model_lod", std::to_string(s.model_lod)},
    {"object_motion", s.object_motion ? "1" : "0"},
    {"vbao", s.vbao ? "1" : "0"},
    {"show_fps", s.show_fps ? "1" : "0"},
    {"effect_chromatic_aberration", s.chromatic_aberration ? "1" : "0"},
    {"effect_dof", s.dof ? "1" : "0"},
    {"effect_motion_blur", s.motion_blur ? "1" : "0"},
    {"effect_bloom", s.bloom ? "1" : "0"},
    {"effect_ssao", s.ssao ? "1" : "0"},
    {"effect_dynamic_shadows", s.dynamic_shadows ? "1" : "0"},
    {"effect_ssr", s.effect_ssr ? "1" : "0"},
    {"skip_intro", s.skip_intro ? "1" : "0"},
    {"debug_camera", s.debug_camera ? "1" : "0"},
    {"debug_menu", s.debug_menu ? "1" : "0"},
    {"low_latency", s.low_latency},
    {"button_prompts", s.button_prompts},
    {"mouse_look", s.mouse_look ? "1" : "0"},
    {"mouse_sensitivity", std::to_string(s.mouse_sensitivity)},
    {"mouse_invert_y", s.mouse_invert_y ? "1" : "0"},
    {"mouse_invert_x", s.mouse_invert_x ? "1" : "0"},
    {"camera_speed_x", std::to_string(s.camera_speed_x)},
    {"camera_speed_y", std::to_string(s.camera_speed_y)}
  };

  std::vector<std::string> out;
  std::unordered_map<std::string, bool> written;

  for (const auto& line : raw_ini_lines_) {
    if (!line.empty() && line[0] != '#' && line[0] != ';' && line.find('=') != std::string::npos) {
      std::string k = line.substr(0, line.find('='));
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
  LauncherSettings copy = settings;
  ValidateSettings(copy);
  SaveSettingsJson(copy);
  SaveIni(copy);
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

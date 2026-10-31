#pragma once

#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bbport {

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  ~UniqueFd() noexcept { Reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }
  int Get() const noexcept { return fd_; }
  bool Valid() const noexcept { return fd_ >= 0; }
  int Release() noexcept {
    int tmp = fd_;
    fd_ = -1;
    return tmp;
  }
  void Reset(int new_fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

struct ProcessHandle {
  pid_t pid = -1;
  int read_fd = -1;

  bool IsValid() const noexcept { return pid > 0; }
};

struct LauncherSettings {
  // Paths & General
  std::string game_dir;
  std::string user_dir = "user";
  std::string mods_dir = "mods";
  std::string patches_dir = "patches";
  std::string ui_language = "en";

  // Display & Graphics
  std::string output_res = "auto";
  bool fullscreen = false;
  bool borderless = false;
  bool vsync = true;
  bool hdr = false;
  std::string aspect_ratio = "auto";
  std::string fps_mode = "uncap";
  int fps_limit = 60;
  std::string upscaler = "fsr4";
  int preset = 4;
  bool sharpen = true;
  float sharpness = 0.50f;
  std::string anti_aliasing = "auto";
  bool vbao = false;
  bool show_fps = true;
  bool object_motion = true;

  // Effects & Engine
  bool motion_blur = true;
  bool bloom = true;
  bool chromatic_aberration = true;
  bool dof = true;
  bool ssao = true;
  bool dynamic_shadows = true;
  std::string low_latency = "on";

  // Controls & Input
  bool mouse_look = true;
  float mouse_sensitivity = 1.0f;
  bool mouse_invert_y = false;
  bool mouse_invert_x = false;
  float camera_speed_x = 1.0f;
  float camera_speed_y = 1.0f;
  std::string button_prompts = "auto";

  // Online & Co-op
  bool online_enabled = false;
  std::string online_pool = "seamless";
  std::string online_server = "play.seamlesshunt.com:31313";
  std::string discord_token;
  bool local_bbsc = true;

  // Developer
  bool vk_validation = false;
  bool save_log = false;
};

class ConfigManager {
public:
  static std::string GetSettingsJsonPath();
  static std::string GetIniPath();

  static LauncherSettings Load();
  static bool Save(const LauncherSettings& settings);

  static bool ValidateGameDirectory(const std::string& path);
  static std::string GetGameVersion(const std::string& path);
  static std::string GetTitleId(const std::string& path);
  static bool IsUpdate109(const std::string& path);

  static int LaunchGame(const LauncherSettings& settings);
  static ProcessHandle LaunchGameAsync(const LauncherSettings& settings);

private:
  static void LoadSettingsJson(LauncherSettings& s);
  static void SaveSettingsJson(const LauncherSettings& s);
  static void LoadIni(LauncherSettings& s);
  static void SaveIni(const LauncherSettings& s);

  static std::vector<std::string> raw_ini_lines_;
};

} // namespace bbport

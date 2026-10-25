#pragma once

#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
  std::string game_dir;
  std::string user_dir = "user";
  std::string mods_dir = "mods";
  std::string output_res = "1920x1080";
  bool fullscreen = false;
  bool borderless = false;
  bool vsync = true;
  int fps_limit = 60;
  std::string upscaler = "native";
  int preset = 0;
  std::string online_pool = "offline";
  std::string online_server = "play.seamlesshunt.com:31313";
  bool mouse_look = true;
  float mouse_sensitivity = 1.0f;
  bool mouse_invert_y = false;
};

class ConfigManager {
public:
  static std::string GetConfigPath();
  static LauncherSettings Load();
  static bool Save(const LauncherSettings& settings);
  static void ApplyToEnvironment(const LauncherSettings& settings);
  static bool ValidateGameDirectory(const std::string& path);
  static std::string GetGameVersion(const std::string& path);
  static std::string GetTitleId(const std::string& path);
  static bool IsUpdate109(const std::string& path);
  static bool ValidateFsr411(const std::string& directory, const std::string& output, int preset);
  static int LaunchGame(const LauncherSettings& settings);
  static ProcessHandle LaunchGameAsync(const LauncherSettings& settings);

private:
  static std::vector<std::string> raw_lines_;
};

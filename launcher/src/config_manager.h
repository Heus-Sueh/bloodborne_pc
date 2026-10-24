#ifndef LAUNCHER_CONFIG_MANAGER_H
#define LAUNCHER_CONFIG_MANAGER_H

#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

struct ProcessHandle {
  pid_t pid = -1;
  int read_fd = -1;
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
  static bool Save(const LauncherSettings &settings);
  static bool ValidateGameDirectory(const std::string &path);
  static bool ValidateFsr411(const std::string &directory,
                             const std::string &output, int preset);
  static int LaunchGame(const LauncherSettings &settings);
  static ProcessHandle LaunchGameAsync(const LauncherSettings &settings);

private:
  static std::vector<std::string> raw_lines_;
};

#endif // LAUNCHER_CONFIG_MANAGER_H

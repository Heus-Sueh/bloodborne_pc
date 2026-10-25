#include "config_manager.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

void test_default_config() {
  LauncherSettings s;
  assert(s.output_res == "1920x1080");
  assert(s.vsync == true);
  assert(s.fps_limit == 60);
  assert(s.online_pool == "offline");
  assert(s.mouse_look == true);
  std::cout << "PASS: test_default_config\n";
}

void test_validation() {
  assert(!ConfigManager::ValidateGameDirectory(""));
  assert(!ConfigManager::ValidateGameDirectory("/nonexistent/directory/12345"));

  fs::path tmp = fs::temp_directory_path() / "bbport_test_game";
  fs::create_directories(tmp);

  assert(!ConfigManager::ValidateGameDirectory(tmp.string()));

  std::ofstream out(tmp / "eboot.bin");
  out << "ELF";
  out.close();

  fs::create_directories(tmp / "dvdroot_ps4" / "shader");
  std::ofstream shader_out(tmp / "dvdroot_ps4" / "shader" / "gxshader.shaderbnd.dcx");
  shader_out << "DCX";
  shader_out.close();

  assert(ConfigManager::ValidateGameDirectory(tmp.string()));
  fs::remove_all(tmp);
  std::cout << "PASS: test_validation\n";
}

void test_config_save_load() {
  fs::path tmp_toml = fs::temp_directory_path() / "test_bbport.toml";
  LauncherSettings s;
  s.output_res = "2560x1440";
  s.fps_limit = 120;
  s.upscaler = "fsr";
  s.online_pool = "seamless";

  // Set custom env or test ConfigManager save
  setenv("BB_CONFIG", tmp_toml.string().c_str(), 1);
  assert(ConfigManager::Save(s));

  LauncherSettings loaded = ConfigManager::Load();
  assert(loaded.output_res == "2560x1440");
  assert(loaded.fps_limit == 120);
  assert(loaded.upscaler == "fsr");
  assert(loaded.online_pool == "seamless");

  fs::remove(tmp_toml);
  unsetenv("BB_CONFIG");
  std::cout << "PASS: test_config_save_load\n";
}

int main() {
  test_default_config();
  test_validation();
  test_config_save_load();
  std::cout << "All launcher tests passed successfully.\n";
  return 0;
}

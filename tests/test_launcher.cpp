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
  s.fullscreen = true;
  s.borderless = true;
  s.vsync = false;
  s.mouse_look = false;
  s.mouse_sensitivity = 2.5f;
  s.mouse_invert_y = true;

  // Set custom env or test ConfigManager save
  setenv("BB_CONFIG", tmp_toml.string().c_str(), 1);
  assert(ConfigManager::Save(s));

  LauncherSettings loaded = ConfigManager::Load();
  assert(loaded.output_res == "2560x1440");
  assert(loaded.fps_limit == 120);
  assert(loaded.upscaler == "fsr");
  assert(loaded.online_pool == "seamless");
  assert(loaded.fullscreen == true);
  assert(loaded.borderless == true);
  assert(loaded.vsync == false);
  assert(loaded.mouse_look == false);
  assert(loaded.mouse_sensitivity == 2.5f);
  assert(loaded.mouse_invert_y == true);

  fs::remove(tmp_toml);
  unsetenv("BB_CONFIG");
  std::cout << "PASS: test_config_save_load\n";
}

void test_apply_to_environment() {
  LauncherSettings s;
  s.game_dir = "/games/bb";
  s.user_dir = "my_user";
  s.fullscreen = true;
  s.borderless = true;
  s.vsync = true;
  s.fps_limit = 90;
  s.output_res = "2560x1440";
  s.upscaler = "fsr4";
  s.preset = 1;
  s.mouse_look = true;
  s.mouse_sensitivity = 1.75f;
  s.mouse_invert_y = false;
  s.online_pool = "huntersdream";
  s.online_server = "server.net:31313";

  ConfigManager::ApplyToEnvironment(s);

  assert(std::string(std::getenv("BB_GAME_DIR")) == "/games/bb");
  assert(std::string(std::getenv("BB_USER_DIR")) == "my_user");
  assert(std::string(std::getenv("BB_FULLSCREEN")) == "1");
  assert(std::string(std::getenv("BB_BORDERLESS")) == "1");
  assert(std::string(std::getenv("BB_VSYNC")) == "1");
  assert(std::string(std::getenv("BB_PRESENT_MODE")) == "Fifo");
  assert(std::string(std::getenv("BB_FPS_LIMIT")) == "90");
  assert(std::string(std::getenv("BB_VBLANK_HZ")) == "90");
  assert(std::string(std::getenv("BB_OUTPUT_RES")) == "2560x1440");
  assert(std::string(std::getenv("BB_UPSCALER")) == "fsr4");
  assert(std::string(std::getenv("BB_PRESET")) == "1");
  assert(std::string(std::getenv("BB_ONLINE")) == "1");
  assert(std::string(std::getenv("BB_NET_BACKEND")) == "thd");
  assert(std::string(std::getenv("BB_SHADNET_SERVER")) == "server.net:31313");
  assert(std::string(std::getenv("BB_MOUSE_LOOK")) == "1");
  assert(std::strtof(std::getenv("BB_MOUSE_SENSITIVITY"), nullptr) == 1.75f);
  assert(std::string(std::getenv("BB_MOUSE_INVERT_Y")) == "0");
  std::cout << "PASS: test_apply_to_environment\n";
}

void test_game_version() {
  assert(ConfigManager::GetGameVersion("").empty());
  assert(ConfigManager::GetTitleId("").empty());
  assert(!ConfigManager::IsUpdate109(""));

  fs::path tmp = fs::temp_directory_path() / "bbport_test_version";
  fs::create_directories(tmp / "sce_sys");

  // Create a minimal mock param.sfo with APP_VER=01.09 and TITLE_ID=CUSA03173
  // PSF header (20 bytes): magic "\0PSF" (4), version (4), key_table_offset (4), val_table_offset
  // (4), count (4)
  std::string sfo_data;
  sfo_data.append("\0PSF", 4);
  uint32_t ver = 0x00000101;
  uint32_t key_off = 20 + 2 * 16; // 2 entries * 16 bytes each
  std::string keys;
  keys.append("APP_VER\0", 8);
  keys.append("TITLE_ID\0", 9);
  while (keys.size() % 4 != 0)
    keys.push_back('\0');
  uint32_t val_off = key_off + keys.size();
  uint32_t count = 2;

  sfo_data.append(reinterpret_cast<const char*>(&ver), 4);
  sfo_data.append(reinterpret_cast<const char*>(&key_off), 4);
  sfo_data.append(reinterpret_cast<const char*>(&val_off), 4);
  sfo_data.append(reinterpret_cast<const char*>(&count), 4);

  std::string val1 = "01.09";
  val1.push_back('\0');
  std::string val2 = "CUSA03173";
  val2.push_back('\0');

  // Entry 0: APP_VER
  uint16_t e0_key_off = 0;
  uint16_t e0_fmt = 0x0204; // string
  uint32_t e0_len = val1.size();
  uint32_t e0_max_len = val1.size();
  uint32_t e0_val_off = 0;
  sfo_data.append(reinterpret_cast<const char*>(&e0_key_off), 2);
  sfo_data.append(reinterpret_cast<const char*>(&e0_fmt), 2);
  sfo_data.append(reinterpret_cast<const char*>(&e0_len), 4);
  sfo_data.append(reinterpret_cast<const char*>(&e0_max_len), 4);
  sfo_data.append(reinterpret_cast<const char*>(&e0_val_off), 4);

  // Entry 1: TITLE_ID
  uint16_t e1_key_off = 8;
  uint16_t e1_fmt = 0x0204;
  uint32_t e1_len = val2.size();
  uint32_t e1_max_len = val2.size();
  uint32_t e1_val_off = val1.size();
  sfo_data.append(reinterpret_cast<const char*>(&e1_key_off), 2);
  sfo_data.append(reinterpret_cast<const char*>(&e1_fmt), 2);
  sfo_data.append(reinterpret_cast<const char*>(&e1_len), 4);
  sfo_data.append(reinterpret_cast<const char*>(&e1_max_len), 4);
  sfo_data.append(reinterpret_cast<const char*>(&e1_val_off), 4);

  // Key table
  sfo_data.append(keys);
  // Value table
  sfo_data.append(val1);
  sfo_data.append(val2);

  std::ofstream sfo_file(tmp / "sce_sys" / "param.sfo", std::ios::binary);
  sfo_file.write(sfo_data.data(), sfo_data.size());
  sfo_file.close();

  assert(ConfigManager::GetGameVersion(tmp.string()) == "01.09");
  assert(ConfigManager::GetTitleId(tmp.string()) == "CUSA03173");
  assert(ConfigManager::IsUpdate109(tmp.string()) == true);

  fs::remove_all(tmp);
  std::cout << "PASS: test_game_version\n";
}

int main() {
  test_default_config();
  test_validation();
  test_config_save_load();
  test_apply_to_environment();
  test_game_version();
  std::cout << "All launcher tests passed successfully.\n";
  return 0;
}

#include "config_manager.hpp"
#include "i18n.hpp"
#include "mods_manager.hpp"
#include "trophy_manager.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace bbport;

void test_default_settings() {
  LauncherSettings s;
  assert(s.ui_language == "en");
  assert(s.present_mode == "Mailbox");
  assert(s.output_res == "auto");
  assert(s.aspect_ratio == "auto");
  assert(s.upscaler == "fsr4");
  assert(s.preset == 4);
  assert(s.frame_generation == "off");
  assert(s.sharpen == true);
  assert(s.sharpness == 0.50f);
  assert(s.anti_aliasing == "auto");
  assert(s.texture_quality == "auto");
  assert(s.model_lod == 0);
  assert(s.object_motion == true);
  assert(s.vbao == false);
  assert(s.show_fps == true);

  assert(s.motion_blur == true);
  assert(s.bloom == true);
  assert(s.chromatic_aberration == true);
  assert(s.dof == true);
  assert(s.ssao == true);
  assert(s.dynamic_shadows == true);
  assert(s.effect_ssr == false);
  assert(s.skip_intro == false);
  assert(s.debug_camera == false);
  assert(s.debug_menu == false);

  assert(s.fps_mode == "uncap");
  assert(s.fps_limit == 0);
  assert(s.low_latency == "on");

  assert(s.button_prompts == "auto");
  assert(s.mouse_look == true);
  assert(s.mouse_sensitivity == 1.0f);
  assert(s.mouse_invert_y == false);
  assert(s.mouse_invert_x == false);
  assert(s.camera_speed_x == 1.0f);
  assert(s.camera_speed_y == 1.0f);

  assert(s.mods_enabled == true);
  assert(s.online_enabled == false);
  assert(s.online_pool == "seamless");
  assert(s.online_server == "play.seamlesshunt.com:31313");
  assert(s.online_upnp == true);

  std::cout << "PASS: test_default_settings\n";
}

void test_settings_validation_and_clamping() {
  LauncherSettings s;
  s.sharpness = 2.5f;       // Should clamp to 1.0
  s.mouse_sensitivity = -5.0f; // Should clamp to 0.1
  s.camera_speed_x = 25.0f; // Should clamp to 10.0
  s.fps_limit = -10;        // Should clamp to 0
  s.preset = 99;            // Should reset to 4
  s.ui_language = "";       // Should default to "en"
  s.upscaler = "";          // Should default to "fsr4"

  ConfigManager::ValidateSettings(s);

  assert(s.sharpness == 1.0f);
  assert(s.mouse_sensitivity == 0.1f);
  assert(s.camera_speed_x == 10.0f);
  assert(s.fps_limit == 0);
  assert(s.preset == 4);
  assert(s.ui_language == "en");
  assert(s.upscaler == "fsr4");

  std::cout << "PASS: test_settings_validation_and_clamping\n";
}

void test_config_save_load() {
  fs::path tmp_dir = fs::temp_directory_path() / "bbport_test_config";
  fs::create_directories(tmp_dir);

  fs::path tmp_json = tmp_dir / "settings.json";
  fs::path tmp_ini = tmp_dir / "bbport.ini";

  setenv("XDG_CONFIG_HOME", tmp_dir.string().c_str(), 1);
  setenv("BB_CONFIG", tmp_ini.string().c_str(), 1);

  LauncherSettings s;
  s.ui_language = "pt_BR";
  s.game_dir = "/path/to/game";
  s.user_dir = "/path/to/user";
  s.mods_dir = "/path/to/mods";
  s.fullscreen = true;
  s.borderless = true;
  s.present_mode = "Fifo";
  s.hdr = true;
  s.aspect_ratio = "21:9";
  s.output_res = "2560x1440";
  s.upscaler = "dlss";
  s.preset = 2;
  s.frame_generation = "dlss";
  s.sharpen = false;
  s.sharpness = 0.85f;
  s.anti_aliasing = "smaa";
  s.texture_quality = "half";
  s.model_lod = 1;
  s.object_motion = false;
  s.vbao = true;
  s.show_fps = false;

  s.chromatic_aberration = false;
  s.dof = false;
  s.motion_blur = false;
  s.bloom = false;
  s.ssao = false;
  s.dynamic_shadows = false;
  s.effect_ssr = true;
  s.skip_intro = true;
  s.debug_camera = true;
  s.debug_menu = true;

  s.fps_mode = "60";
  s.fps_limit = 60;
  s.low_latency = "boost";
  s.draw_pipe = "1";

  s.button_prompts = "playstation";
  s.mouse_look = false;
  s.mouse_sensitivity = 2.2f;
  s.mouse_invert_y = true;
  s.mouse_invert_x = true;
  s.camera_speed_x = 1.5f;
  s.camera_speed_y = 1.8f;

  s.mods_enabled = false;
  s.online_enabled = true;
  s.online_pool = "huntersdream";
  s.online_server = "custom.server.com:31313";
  s.online_npid = "TestHunter";
  s.online_password = "SecretPassword";
  s.discord_token = "abc123token";
  s.online_upnp = false;

  s.save_log = true;
  s.frame_stats = true;
  s.gpu_profile = true;
  s.crash_diag = true;
  s.vk_validation = true;
  s.mangohud = true;
  s.extra_env = "MESA_VK_WSI_PRESENT_MODE=mailbox";

  assert(ConfigManager::Save(s));

  // Load back and verify all fields
  LauncherSettings loaded = ConfigManager::Load();
  assert(loaded.ui_language == "pt_BR");
  assert(loaded.game_dir == "/path/to/game");
  assert(loaded.user_dir == "/path/to/user");
  assert(loaded.mods_dir == "/path/to/mods");
  assert(loaded.fullscreen == true);
  assert(loaded.borderless == true);
  assert(loaded.present_mode == "Fifo");
  assert(loaded.hdr == true);
  assert(loaded.aspect_ratio == "21:9");
  assert(loaded.output_res == "2560x1440");
  assert(loaded.upscaler == "dlss");
  assert(loaded.preset == 2);
  assert(loaded.frame_generation == "dlss");
  assert(loaded.sharpen == false);
  assert(loaded.sharpness == 0.85f);
  assert(loaded.anti_aliasing == "smaa");
  assert(loaded.texture_quality == "half");
  assert(loaded.model_lod == 1);
  assert(loaded.object_motion == false);
  assert(loaded.vbao == true);
  assert(loaded.show_fps == false);

  assert(loaded.chromatic_aberration == false);
  assert(loaded.dof == false);
  assert(loaded.motion_blur == false);
  assert(loaded.bloom == false);
  assert(loaded.ssao == false);
  assert(loaded.dynamic_shadows == false);
  assert(loaded.effect_ssr == true);
  assert(loaded.skip_intro == true);
  assert(loaded.debug_camera == true);
  assert(loaded.debug_menu == true);

  assert(loaded.fps_mode == "60");
  assert(loaded.fps_limit == 60);
  assert(loaded.low_latency == "boost");
  assert(loaded.draw_pipe == "1");

  assert(loaded.button_prompts == "playstation");
  assert(loaded.mouse_look == false);
  assert(loaded.mouse_sensitivity == 2.2f);
  assert(loaded.mouse_invert_y == true);
  assert(loaded.mouse_invert_x == true);
  assert(loaded.camera_speed_x == 1.5f);
  assert(loaded.camera_speed_y == 1.8f);

  assert(loaded.mods_enabled == false);
  assert(loaded.online_enabled == true);
  assert(loaded.online_pool == "huntersdream");
  assert(loaded.online_server == "custom.server.com:31313");
  assert(loaded.online_npid == "TestHunter");
  assert(loaded.online_password == "SecretPassword");
  assert(loaded.discord_token == "abc123token");
  assert(loaded.online_upnp == false);

  assert(loaded.save_log == true);
  assert(loaded.frame_stats == true);
  assert(loaded.gpu_profile == true);
  assert(loaded.crash_diag == true);
  assert(loaded.vk_validation == true);
  assert(loaded.mangohud == true);
  assert(loaded.extra_env == "MESA_VK_WSI_PRESENT_MODE=mailbox");

  fs::remove_all(tmp_dir);
  unsetenv("XDG_CONFIG_HOME");
  unsetenv("BB_CONFIG");
  std::cout << "PASS: test_config_save_load\n";
}

void test_mods_manager() {
  fs::path tmp_mods = fs::temp_directory_path() / "bbport_test_mods";
  fs::create_directories(tmp_mods / "Mod_60FPS");
  fs::create_directories(tmp_mods / "Mod_Textures");
  fs::create_directories(tmp_mods / "Mod_Audio");

  fs::path tmp_patches = fs::temp_directory_path() / "bbport_test_patches";
  fs::create_directories(tmp_patches);

  std::ofstream patch_file(tmp_patches / "patch1.xml");
  patch_file << "<patch isEnabled=\"true\">\n"
             << "  <title>No Motion Blur</title>\n"
             << "  <author>FromSoftFan</author>\n"
             << "</patch>\n";
  patch_file.close();

  ModsManager& mgr = ModsManager::Instance();
  mgr.Refresh(tmp_mods.string(), tmp_patches.string());

  assert(mgr.GetMods().size() == 3);
  assert(mgr.GetPatches().size() == 1);
  assert(mgr.GetPatches()[0].name == "No Motion Blur");
  assert(mgr.GetPatches()[0].author == "FromSoftFan");
  assert(mgr.GetPatches()[0].enabled == true);

  // Reordering
  std::string first_id = mgr.GetMods()[0].id;
  std::string second_id = mgr.GetMods()[1].id;
  assert(mgr.MoveModDown(first_id));
  assert(mgr.GetMods()[0].id == second_id);
  assert(mgr.GetMods()[1].id == first_id);

  // Toggle
  assert(mgr.ToggleMod(first_id));
  assert(mgr.GetMods()[1].enabled == false);

  // Save and reload
  assert(mgr.SaveModsConfig(tmp_mods.string()));
  mgr.Refresh(tmp_mods.string(), tmp_patches.string());
  assert(mgr.GetMods()[1].id == first_id);
  assert(mgr.GetMods()[1].enabled == false);

  fs::remove_all(tmp_mods);
  fs::remove_all(tmp_patches);
  std::cout << "PASS: test_mods_manager\n";
}

void test_trophy_manager() {
  fs::path tmp_user = fs::temp_directory_path() / "bbport_test_user";
  fs::path trophy_dir = tmp_user / "trophy" / "NPWR05818_00";
  fs::create_directories(trophy_dir);

  std::ofstream trop_xml(trophy_dir / "TROP.XML");
  trop_xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           << "<trophyconf version=\"1.1\" npcommid=\"NPWR05818_00\">\n"
           << "  <trophy id=\"0\" hidden=\"no\" ttype=\"P\">\n"
           << "    <name>Bloodborne</name>\n"
           << "    <detail>All trophies acquired. Hath awakened from the hunt.</detail>\n"
           << "  </trophy>\n"
           << "  <trophy id=\"1\" hidden=\"no\" ttype=\"G\">\n"
           << "    <name>Childhood's Beginning</name>\n"
           << "    <detail>Became an infant Great One.</detail>\n"
           << "  </trophy>\n"
           << "  <trophy id=\"2\" hidden=\"no\" ttype=\"B\">\n"
           << "    <name>Cleric Beast</name>\n"
           << "    <detail>Defeat Cleric Beast.</detail>\n"
           << "  </trophy>\n"
           << "</trophyconf>\n";
  trop_xml.close();

  std::ofstream unlocked_txt(trophy_dir / "unlocked.txt");
  unlocked_txt << "2\n"; // Only Cleric Beast unlocked
  unlocked_txt.close();

  std::ofstream jsonl(tmp_user / "trophies.jsonl");
  jsonl << "{\"id\": 2, \"time\": \"2026-10-31 10:30:00\"}\n";
  jsonl.close();

  TrophyManager& tm = TrophyManager::Instance();
  tm.Refresh(tmp_user.string(), "en");

  assert(tm.GetTrophies().size() == 3);
  assert(tm.GetStats().total == 3);
  assert(tm.GetStats().unlocked == 1);
  assert(tm.GetStats().progress_pct == 33);
  assert(tm.GetStats().platinum_total == 1);
  assert(tm.GetStats().platinum_unlocked == 0);
  assert(tm.GetStats().bronze_total == 1);
  assert(tm.GetStats().bronze_unlocked == 1);

  assert(tm.GetTrophies()[0].grade == TrophyGrade::Platinum);
  assert(tm.GetTrophies()[0].unlocked == false);
  assert(tm.GetTrophies()[2].grade == TrophyGrade::Bronze);
  assert(tm.GetTrophies()[2].unlocked == true);
  assert(tm.GetTrophies()[2].date_str == "2026-10-31 10:30:00");

  fs::remove_all(tmp_user);
  std::cout << "PASS: test_trophy_manager\n";
}

void test_i18n() {
  I18n& i18n = I18n::Instance();

  i18n.SetLanguage(Language::English);
  assert(i18n.Translate("tab_play") == "The Hunt");
  assert(i18n.Translate("launch_game") == "LAUNCH GAME");

  i18n.SetLanguage(Language::Portuguese);
  assert(i18n.Translate("tab_play") == "A Caçada");
  assert(i18n.Translate("launch_game") == "INICIAR JOGO");

  i18n.SetLanguage(Language::Russian);
  assert(i18n.Translate("tab_play") == "Игра");
  assert(i18n.Translate("launch_game") == "ЗАПУСТИТЬ");

  i18n.SetLanguage(Language::Chinese);
  assert(i18n.Translate("tab_play") == "游戏");
  assert(i18n.Translate("launch_game") == "启动游戏");

  i18n.SetLanguage(Language::French);
  assert(i18n.Translate("tab_play") == "La Chasse");
  assert(i18n.Translate("launch_game") == "LANCER LE JEU");

  // Fallback test
  assert(i18n.Translate("nonexistent_key_123") == "nonexistent_key_123");

  std::cout << "PASS: test_i18n\n";
}

int main() {
  std::cout << "=== Running Bloodborne Launcher Test Suite ===\n";
  test_default_settings();
  test_settings_validation_and_clamping();
  test_config_save_load();
  test_mods_manager();
  test_trophy_manager();
  test_i18n();
  std::cout << "=== All 6 Test Suites Passed Successfully (100% Success Rate) ===\n";
  return 0;
}

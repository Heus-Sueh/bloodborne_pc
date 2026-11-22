// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "launcher_core.hpp"
#include "launch_env.hpp"
#include "process_runner.hpp"
#include "settings_store.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace bbport;

namespace {

fs::path RepoRoot() {
#ifdef BBPORT_REPO_ROOT
  return fs::path(BBPORT_REPO_ROOT);
#else
  return fs::current_path();
#endif
}

struct TempWorkspace {
  fs::path root;
  PortPaths paths;

  explicit TempWorkspace(const std::string& tag) {
    root = fs::temp_directory_path() / ("bbport_deep_test_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(root);
    paths.port_dir = root / "port";
    paths.data_dir = root / "data";
    paths.config_dir = root / "config";

    fs::create_directories(paths.port_dir / "launcher");
    fs::create_directories(paths.data_dir);
    fs::create_directories(paths.config_dir);

    // Copy real settings_schema.json from repo into Port Directory
    fs::copy_file(RepoRoot() / "launcher" / "settings_schema.json", paths.SchemaPath());

    // Create stub run.sh in Port Directory
    std::ofstream runner(paths.RunnerScriptPath());
    runner << "#!/usr/bin/env bash\nexit 0\n";
    runner.close();
  }

  ~TempWorkspace() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// Build a real binary PS4 param.sfo (\0PSF) matching scripts/prepare.py::sfo().
void WriteParamSfo(const fs::path& sfo_path, const std::vector<std::pair<std::string, std::string>>& entries) {
  fs::create_directories(sfo_path.parent_path());

  std::vector<std::uint8_t> key_table;
  std::vector<std::uint8_t> val_table;
  struct EntryMeta {
    std::uint16_t key_off;
    std::uint32_t len;
    std::uint32_t max_len;
    std::uint32_t val_off;
  };
  std::vector<EntryMeta> metas;

  for (const auto& [k, v] : entries) {
    EntryMeta m{};
    m.key_off = static_cast<std::uint16_t>(key_table.size());
    key_table.insert(key_table.end(), k.begin(), k.end());
    key_table.push_back(0);

    m.val_off = static_cast<std::uint32_t>(val_table.size());
    m.len = static_cast<std::uint32_t>(v.size() + 1);
    m.max_len = (m.len + 3u) & ~3u;
    val_table.insert(val_table.end(), v.begin(), v.end());
    val_table.resize(m.val_off + m.max_len, 0);
    metas.push_back(m);
  }
  while (key_table.size() % 4 != 0) key_table.push_back(0);

  const std::uint32_t key_table_start = 20 + static_cast<std::uint32_t>(metas.size() * 16);
  const std::uint32_t val_table_start = key_table_start + static_cast<std::uint32_t>(key_table.size());

  std::vector<std::uint8_t> out;
  auto push_u16 = [&](std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  };
  auto push_u32 = [&](std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
  };

  out.insert(out.end(), {0x00, 'P', 'S', 'F'});
  push_u32(0x00000101);
  push_u32(key_table_start);
  push_u32(val_table_start);
  push_u32(static_cast<std::uint32_t>(metas.size()));

  for (const auto& m : metas) {
    push_u16(m.key_off);
    push_u16(0x0204); // UTF-8 null-terminated
    push_u32(m.len);
    push_u32(m.max_len);
    push_u32(m.val_off);
  }
  out.insert(out.end(), key_table.begin(), key_table.end());
  out.insert(out.end(), val_table.begin(), val_table.end());

  std::ofstream f(sfo_path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

void SeedFakeGameDir(const fs::path& dir, const std::string& title_id, const std::string& app_ver) {
  fs::create_directories(dir / "dvdroot_ps4");
  std::ofstream eboot(dir / "eboot.bin", std::ios::binary);
  eboot << "SELF";
  WriteParamSfo(dir / "sce_sys" / "param.sfo", {{"APP_VER", app_ver}, {"TITLE_ID", title_id}});
}

class FakeSystemProbe final : public SystemProbe {
public:
  std::vector<GpuInfo> gpus;
  bool has_online = false;
  std::map<std::string, std::string> host_env;
  std::string sys_lang = "en";

  std::vector<GpuInfo> ListGpus() const override { return gpus; }
  bool HasOnlineModule(const PortPaths&, std::string_view) const override { return has_online; }
  std::map<std::string, std::string> HostEnvironment() const override { return host_env; }
  std::string DetectSystemLanguage() const override { return sys_lang; }
};

class ManualClock final : public Clock {
public:
  std::chrono::steady_clock::time_point now_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point Now() const override { return now_; }
  void Advance(std::chrono::seconds s) { now_ += s; }
};

struct FakeSessionState {
  SpawnSpec spec;
  std::vector<std::string> pending_lines;
  std::optional<int> exit_code;
  bool terminated = false;
};

class FakeProcessSession final : public ProcessSession {
public:
  explicit FakeProcessSession(std::shared_ptr<FakeSessionState> st) : st_(std::move(st)) {}

  void DrainOutput(std::vector<std::string>& out_lines) override {
    out_lines.insert(out_lines.end(), st_->pending_lines.begin(), st_->pending_lines.end());
    st_->pending_lines.clear();
  }
  std::optional<int> TryWait() override { return st_->exit_code; }
  void Terminate(std::chrono::milliseconds) override {
    st_->terminated = true;
    if (!st_->exit_code.has_value()) {
      st_->exit_code = 143;
    }
  }

private:
  std::shared_ptr<FakeSessionState> st_;
};

class FakeProcessRunner final : public ProcessRunner {
public:
  std::vector<std::shared_ptr<FakeSessionState>> spawned;
  std::string next_error;

  SpawnResult Spawn(const SpawnSpec& spec) override {
    if (!next_error.empty()) {
      std::string e = next_error;
      next_error.clear();
      return {nullptr, e};
    }
    auto st = std::make_shared<FakeSessionState>();
    st->spec = spec;
    spawned.push_back(st);
    return {std::make_unique<FakeProcessSession>(st), {}};
  }
};

mode_t FileMode(const fs::path& p) {
  struct stat st{};
  assert(::stat(p.c_str(), &st) == 0);
  return st.st_mode & 0777;
}

// ============================================================================
// Test 1: Schema-driven SettingsStore (clamps, parity, unknown preservation, 0600, escaping)
// ============================================================================
void TestSettingsStoreContract() {
  TempWorkspace ws("settings");
  SettingsSchema schema;
  std::string err;
  assert(SettingsSchema::LoadFromFile(ws.paths.SchemaPath(), schema, &err));
  assert(schema.Find("sharpness") != nullptr);
  assert(schema.Find("online") != nullptr);
  assert(schema.Find("online_bbsc") != nullptr);
  assert(schema.Find("borderless") == nullptr);

  // Pre-seed unknown keys & comments in both files + legacy borderless + string "language": "1"
  {
    std::ofstream js(ws.paths.SettingsJsonPath());
    js << R"({"language": "8", "borderless": true, "gtk_future_key": "keep_me", "online_server": "srv.shadps4.net:31313"})";
  }
  {
    std::ofstream ini(ws.paths.IniPath());
    ini << "# in-game comment\n";
    ini << "custom_in_game_key = 42\n";
    ini << "sharpness = 9.99\n";
  }

  SettingsStore store(schema, ws.paths);
  LoadReport rep;
  assert(store.Load(&rep));

  // Verify schema clamping, GTK string language parity, and legacy online migration
  assert(store.GetString("language") == "8");
  assert(store.GetDouble("sharpness") == 2.0); // schema max is 2.0 (not old C++ 1.0!)
  assert(store.GetString("online_server") == "play.seamlesshunt.com:31313");

  // Verify camera_speed clamping matches schema [0.25, 2.0]
  assert(store.Set("camera_speed_x", 10.0));
  assert(store.GetDouble("camera_speed_x") == 2.0);
  assert(store.Set("camera_speed_y", 0.01));
  assert(store.GetDouble("camera_speed_y") == 0.25);
  assert(store.Set("fps_limit", -50));
  assert(store.GetInt("fps_limit") == 0);

  // Quotes and backslashes in passwords / extra_env must survive JSON round-trip safely
  assert(store.Set("online_password", std::string("p@ss\"with\\quote")));
  assert(store.Set("extra_env", std::string("FOO=\"bar\" BAZ=1")));
  assert(store.Set("online", true));
  assert(store.Set("online_bbsc", false));
  assert(store.Set("thd_online_id", std::string("Hunter42")));
  assert(store.Set("thd_token", std::string("tok_secret")));
  assert(store.Save(&err));

  // settings.json must have 0600 permissions
  assert(FileMode(ws.paths.SettingsJsonPath()) == 0600);

  // Reload and verify unknown keys + comments survived and obsolete borderless was dropped
  SettingsStore reloaded(schema, ws.paths);
  assert(reloaded.Load());
  assert(reloaded.GetString("online_password") == "p@ss\"with\\quote");
  assert(reloaded.GetString("extra_env") == "FOO=\"bar\" BAZ=1");
  assert(reloaded.GetBool("online") == true);
  assert(reloaded.GetBool("online_bbsc") == false);
  assert(reloaded.GetString("thd_online_id") == "Hunter42");
  assert(reloaded.GetString("thd_token") == "tok_secret");

  std::ifstream js_in(ws.paths.SettingsJsonPath());
  std::string js_text((std::istreambuf_iterator<char>(js_in)), {});
  assert(js_text.find("gtk_future_key") != std::string::npos);
  assert(js_text.find("borderless") == std::string::npos);

  std::ifstream ini_in(ws.paths.IniPath());
  std::string ini_text((std::istreambuf_iterator<char>(ini_in)), {});
  assert(ini_text.find("# in-game comment") != std::string::npos);
  assert(ini_text.find("custom_in_game_key = 42") != std::string::npos);

  std::cout << "PASS: TestSettingsStoreContract\n";
}

// ============================================================================
// Test 2: LaunchEnvCompiler (declarative 1:1 BB_* + all 6 multi-var/conditional rules)
// ============================================================================
void TestLaunchEnvCompiler() {
  TempWorkspace ws("env");
  SettingsSchema schema;
  assert(SettingsSchema::LoadFromFile(ws.paths.SchemaPath(), schema));

  SettingsStore store(schema, ws.paths);
  assert(store.Load());
  assert(store.Set("game_dir", std::string("/games/bb")));
  assert(store.Set("user_dir", std::string("/saves/bb")));
  assert(store.Set("language", std::string("8")));
  assert(store.Set("ui_language", std::string("pt_BR")));
  assert(store.Set("fullscreen", true));
  assert(store.Set("present_mode", std::string("Fifo")));
  assert(store.Set("hdr", true));
  assert(store.Set("fps_mode", std::string("60")));
  assert(store.Set("fps_limit", 120));
  assert(store.Set("draw_pipe", std::string("1")));
  assert(store.Set("display", std::string("DP-1")));
  assert(store.Set("gamepad", std::string("DualSense")));
  assert(store.Set("gpu", std::string("AMD RX 7900 XTX")));
  assert(store.Set("mangohud", true));
  assert(store.Set("frame_stats", true));
  assert(store.Set("crash_diag", true));
  assert(store.Set("vk_validation", true));
  assert(store.Set("online", true));
  assert(store.Set("online_pool", std::string("huntersdream")));
  assert(store.Set("thd_server", std::string("thehuntersdream.com")));
  assert(store.Set("thd_online_id", std::string("GoodHunter")));
  assert(store.Set("thd_token", std::string("secret_tok")));
  assert(store.Set("trophy_key", std::string("0123456789abcdef0123456789ABCDEF")));
  assert(store.Set("extra_env", std::string("BB_CUSTOM=yes BB_FPS=90")));

  FakeSystemProbe probe;
  probe.gpus = {{0, "Intel iGPU", false}, {1, "AMD RX 7900 XTX", true}};
  probe.has_online = true;
  probe.host_env = {{"MANGOHUD_CONFIG", "user_custom"}};

  const LaunchPlan plan = LaunchEnvCompiler::Compile(ws.paths, store, probe);
  assert(plan.env.at("BB_GAME_DIR") == "/games/bb");
  assert(plan.env.at("BB_USER_DIR") == "/saves/bb");
  assert(plan.env.at("BB_MODS_CONFIG") == ws.paths.ModsConfigPath().string());
  assert(plan.env.at("BB_PATCHES_CONFIG") == ws.paths.PatchesConfigPath().string());
  assert(plan.env.at("BB_LANGUAGE") == "8"); // PS4 numeric language, NOT game_text_language
  assert(plan.env.at("BB_MENU_LANGUAGE") == "pt_BR");
  assert(plan.env.at("BB_FULLSCREEN") == "1");
  assert(plan.env.at("BB_PRESENT_MODE") == "Fifo");
  assert(plan.env.at("BB_HDR") == "1");
  assert(plan.env.at("BB_FPS_LIMIT") == "120");
  assert(plan.env.at("BB_DRAW_PIPE") == "1");
  assert(plan.env.at("BB_DISPLAY") == "DP-1");
  assert(plan.env.at("BB_GAMEPAD") == "DualSense");
  assert(plan.env.at("BB_GPU_ID") == "1");
  assert(plan.env.at("MANGOHUD") == "1");
  assert(plan.env.at("MANGOHUD_CONFIG") == "user_custom"); // shell env wins
  assert(plan.env.at("BB_FRAME_STATS") == "1");
  assert(plan.env.at("BB_FREE_CHECK") == "1");
  assert(plan.env.at("BB_WRITE_LOG") == "1");
  assert(plan.env.at("BB_PRODUCER_CHECK") == "1");
  assert(plan.env.at("BB_VK_VALIDATION") == "1");
  assert(plan.env.at("BB_ONLINE") == "1");
  assert(plan.env.at("BB_NET_BACKEND") == "thd");
  assert(plan.env.at("BB_COMMUNITY_SERVER") == "https://thehuntersdream.com");
  assert(plan.env.at("BB_THD_ONLINE_ID") == "GoodHunter");
  assert(plan.env.at("BB_THD_TOKEN") == "secret_tok");
  assert(plan.env.at("BB_TROPHY_KEY") == "0123456789abcdef0123456789ABCDEF");
  assert(plan.env.at("BB_CUSTOM") == "yes");
  assert(plan.env.at("BB_FPS") == "90"); // extra_env override applied last

  // When online module is absent, BB_ONLINE must NOT be emitted
  probe.has_online = false;
  const LaunchPlan offline_plan = LaunchEnvCompiler::Compile(ws.paths, store, probe);
  assert(offline_plan.env.find("BB_ONLINE") == offline_plan.env.end());

  std::cout << "PASS: TestLaunchEnvCompiler\n";
}

// ============================================================================
// Test 3: Single-pass Game Directory SFO inspection (1.09 vs 1.00 + nested CUSA subdir)
// ============================================================================
void TestGameDirectoryInspection() {
  TempWorkspace ws("game_sfo");
  const fs::path game_109 = ws.root / "games_root" / "CUSA03173";
  SeedFakeGameDir(game_109, "CUSA03173", "01.09");

  // Parent directory resolves nested CUSA03173 automatically
  const GameInspection r109 = InspectGameDirectory(ws.root / "games_root");
  assert(r109.valid);
  assert(r109.is_109);
  assert(r109.title_id == "CUSA03173");
  assert(r109.version == "01.09");
  assert(r109.status_key == "status_ready");

  const fs::path game_100 = ws.root / "game_100";
  SeedFakeGameDir(game_100, "CUSA00900", "01.00");
  const GameInspection r100 = InspectGameDirectory(game_100);
  assert(r100.valid);
  assert(!r100.is_109);
  assert(r100.title_id == "CUSA00900");
  assert(r100.version == "01.00");
  assert(r100.status_key == "status_update_needed");

  std::cout << "PASS: TestGameDirectoryInspection\n";
}

// ============================================================================
// Test 4: Real Mods & External Patches on-disk contracts (DATA_DIR/{mods,patches}.json + shadPS4 XML)
// ============================================================================
void TestModsAndExternalPatchesContracts() {
  TempWorkspace ws("content");
  const fs::path mods_dir = ws.paths.data_dir / "mods";
  const fs::path patches_dir = ws.paths.data_dir / "patches";

  // Valid mod layouts: dvdroot_ps4 + bare chr/ folder; invalid random folder must be ignored
  fs::create_directories(mods_dir / "Mod_Alpha" / "dvdroot_ps4" / "chr");
  fs::create_directories(mods_dir / "Mod_Beta" / "chr");
  fs::create_directories(mods_dir / "NotAMod");
  {
    std::ofstream f(mods_dir / "NotAMod" / "random.bin");
    f << "junk";
  }

  // Real shadPS4 XML patch format + built-in Bloodborne.xml excluded
  fs::create_directories(patches_dir);
  fs::create_directories(ws.paths.port_dir / "patches");
  {
    std::ofstream builtin(ws.paths.BuiltinPatchesXmlPath());
    builtin << "<Patch><ID>CUSA03173</ID><Metadata Name=\"BuiltIn\" AppVer=\"01.09\" isEnabled=\"true\"/></Patch>\n";
  }
  {
    std::ofstream xml(patches_dir / "community.xml");
    xml << R"(<?xml version="1.0" encoding="utf-8"?>
<Patch>
  <ID>CUSA03173</ID>
  <Metadata Title="Bloodborne" Name="60 FPS Delta" Author="ModderA" AppVer="01.09" AppElf="eboot.bin" isEnabled="false"/>
  <Metadata Title="Bloodborne" Name="Skip Intro" Author="ModderB" AppVer="01.09" isEnabled="true"/>
  <Metadata Title="Bloodborne" Name="Old 1.00 Patch" Author="Old" AppVer="01.00" isEnabled="true"/>
</Patch>)";
  }
  // Seed pre-existing override from another folder to ensure SaveExternalPatchesConfig preserves it
  {
    std::ofstream pf(ws.paths.PatchesConfigPath());
    pf << R"({"enabled": ["other.xml/KeepMe"], "disabled": []})";
  }

  auto mods = LoadMods(mods_dir, ws.paths.ModsConfigPath());
  assert(mods.size() == 2);
  assert(mods[0].id == "Mod_Alpha");
  assert(mods[1].id == "Mod_Beta");
  std::swap(mods[0], mods[1]);
  mods[0].enabled = false;
  assert(SaveModsConfig(ws.paths.ModsConfigPath(), mods));

  auto mods_reloaded = LoadMods(mods_dir, ws.paths.ModsConfigPath());
  assert(mods_reloaded.size() == 2);
  assert(mods_reloaded[0].id == "Mod_Beta" && !mods_reloaded[0].enabled);
  assert(mods_reloaded[1].id == "Mod_Alpha" && mods_reloaded[1].enabled);

  auto patches = LoadExternalPatches(patches_dir, ws.paths.PatchesConfigPath(), ws.paths.BuiltinPatchesXmlPath());
  assert(patches.size() == 2);
  assert(patches[0].id == "community.xml/60 FPS Delta" && !patches[0].enabled);
  assert(patches[1].id == "community.xml/Skip Intro" && patches[1].enabled);

  // Flip both patches so both differ from XML isEnabled defaults
  patches[0].enabled = true;
  patches[1].enabled = false;
  assert(SaveExternalPatchesConfig(ws.paths.PatchesConfigPath(), patches));

  // Verify delta format + preserved external override
  std::ifstream pf_in(ws.paths.PatchesConfigPath());
  std::string pf_text((std::istreambuf_iterator<char>(pf_in)), {});
  assert(pf_text.find("community.xml/60 FPS Delta") != std::string::npos);
  assert(pf_text.find("community.xml/Skip Intro") != std::string::npos);
  assert(pf_text.find("other.xml/KeepMe") != std::string::npos);

  auto patches_reloaded = LoadExternalPatches(patches_dir, ws.paths.PatchesConfigPath(), ws.paths.BuiltinPatchesXmlPath());
  assert(patches_reloaded[0].enabled == true);
  assert(patches_reloaded[1].enabled == false);

  std::cout << "PASS: TestModsAndExternalPatchesContracts\n";
}

// ============================================================================
// Test 5: Real Trophies contract (BbTrophy XML entity decoding + TROP_08.XML + trophies.jsonl)
// ============================================================================
void TestTrophiesRealContract() {
  TempWorkspace ws("trophies");
  const fs::path saves_dir = ws.paths.data_dir / "user";
  const fs::path trop_dir = saves_dir / "trophy" / "NPWR05818_00";
  fs::create_directories(trop_dir);

  // PS4 numeric Russian file TROP_08.XML with XML entities (&amp;, &apos;)
  {
    std::ofstream xml(trop_dir / "TROP_08.XML");
    xml << R"(<?xml version="1.0" encoding="utf-8"?>
<trophyconf version="1.1">
  <trophy id="0" hidden="no" ttype="P"><name>Bloodborne</name><detail>All trophies</detail></trophy>
  <trophy id="12" hidden="no" ttype="B"><name>Hunter&apos;s Craft &amp; Blood</name><detail>Acquire tools</detail></trophy>
</trophyconf>)";
  }
  // Real src/runtime_trophy.c JSONL schema: trophy_id + unlocked_at
  {
    std::ofstream jl(saves_dir / "trophies.jsonl");
    jl << R"({"v":1,"title_id":"CUSA03173","trophy_id":12,"unlocked_at":"2026-10-10T06:30:00Z"})" << "\n";
  }

  const TrophyCatalog cat = LoadTrophies(saves_dir, "8");
  assert(cat.named);
  assert(cat.trophies.size() == 2);
  assert(cat.stats.total == 2 && cat.stats.unlocked == 1 && cat.stats.progress_pct == 50);
  assert(cat.trophies[1].id == 12);
  assert(cat.trophies[1].name == "Hunter's Craft & Blood"); // entity decoded by BbTrophy
  assert(cat.trophies[1].unlocked == true);
  assert(cat.trophies[1].date_str == "2026-10-10T06:30:00Z");

  // Unnamed fallback when no TROP.XML exists yet
  const fs::path bare_saves = ws.root / "bare_saves";
  fs::create_directories(bare_saves);
  {
    std::ofstream jl(bare_saves / "trophies.jsonl");
    jl << R"({"v":1,"title_id":"CUSA03173","trophy_id":7,"unlocked_at":"2026-11-01T12:00:00Z"})" << "\n";
  }
  const TrophyCatalog unnamed = LoadTrophies(bare_saves, "1");
  assert(!unnamed.named);
  assert(unnamed.trophies.size() == 1);
  assert(unnamed.trophies[0].id == 7 && unnamed.trophies[0].unlocked);

  std::cout << "PASS: TestTrophiesRealContract\n";
}

// ============================================================================
// Test 6: LauncherCore lifecycle, cross-thread inbox, quick-crash focus, extraction safety
// ============================================================================
void TestLauncherCoreLifecycleAndSafety() {
  TempWorkspace ws("core");
  const fs::path game_dir = ws.root / "game_109";
  SeedFakeGameDir(game_dir, "CUSA03173", "01.09");

  auto fake_runner = std::make_unique<FakeProcessRunner>();
  auto* runner_ptr = fake_runner.get();
  auto fake_probe = std::make_unique<FakeSystemProbe>();
  fake_probe->has_online = true;
  auto manual_clock = std::make_unique<ManualClock>();
  auto* clock_ptr = manual_clock.get();

  std::unique_ptr<LauncherCore> core;
  auto opened = LauncherCore::Open(
      ws.paths,
      {std::move(fake_runner), std::move(fake_probe), std::move(manual_clock)},
      core);
  assert(opened && core != nullptr);

  // Thread-safe cross-thread inbox (simulating SDL dialog thread)
  core->PostSetting("game_dir", game_dir.string());
  const StateDelta d_inbox = core->Poll();
  assert(d_inbox.Contains("game_dir"));
  assert(d_inbox.Contains("is_valid"));
  assert(core->GameStatus().valid && core->GameStatus().is_109);

  // Extraction must fail cleanly when no extractor tool exists under Port Directory (never fake success!)
  const ActionResult ex_missing = core->StartExtraction("/tmp/fake.pkg", ws.root / "out_extract");
  assert(!ex_missing);
  assert(!core->Extraction().active);
  assert(ex_missing.delta.focus_log_tab);

  // Quick-crash detection (<4s non-zero exit -> focus_log_tab=true)
  assert(core->LaunchGame());
  assert(core->GameProcess().running);
  assert(runner_ptr->spawned.size() == 1);
  runner_ptr->spawned.back()->pending_lines.push_back("vkCreateDevice failed");
  clock_ptr->Advance(std::chrono::seconds(2));
  runner_ptr->spawned.back()->exit_code = 1;
  const StateDelta d_crash = core->Poll();
  assert(!core->GameProcess().running);
  assert(d_crash.focus_log_tab);
  assert(core->ConsoleLog().find("vkCreateDevice failed") != std::string::npos);

  // Normal exit (>4s) does NOT focus log tab
  assert(core->LaunchGame());
  clock_ptr->Advance(std::chrono::seconds(10));
  runner_ptr->spawned.back()->exit_code = 0;
  const StateDelta d_ok = core->Poll();
  assert(!core->GameProcess().running);
  assert(!d_ok.focus_log_tab);

  // StopGame terminates running session cleanly
  assert(core->LaunchGame());
  auto active_session = runner_ptr->spawned.back();
  assert(core->StopGame());
  assert(active_session->terminated);
  assert(!core->GameProcess().running);

  // When extractor script exists, successful extraction updates game_dir automatically
  fs::create_directories(ws.paths.port_dir / "tools" / "extractor");
  {
    std::ofstream ex(ws.paths.port_dir / "tools" / "extractor" / "extract_dump.sh");
    ex << "#!/usr/bin/env bash\nexit 0\n";
  }
  const fs::path extracted_dir = ws.root / "extracted_game";
  SeedFakeGameDir(extracted_dir, "CUSA03173", "01.09");
  assert(core->StartExtraction("/tmp/game.pkg", extracted_dir));
  assert(core->Extraction().active);
  runner_ptr->spawned.back()->pending_lines.push_back("Extracting 100%");
  runner_ptr->spawned.back()->exit_code = 0;
  const StateDelta d_ex = core->Poll();
  assert(!core->Extraction().active);
  assert(core->Extraction().progress_pct == 100);
  assert(core->Settings().GetString("game_dir") == extracted_dir.string());
  assert(d_ex.Contains("game_dir"));

  std::cout << "PASS: TestLauncherCoreLifecycleAndSafety\n";
}

// ============================================================================
// Test 7: Real PosixProcessRunner smoke test (fork/setsid/execve/UniqueFd/killpg)
// ============================================================================
void TestPosixProcessRunnerSmoke() {
  PosixProcessRunner runner;
  SpawnSpec spec;
  spec.executable = "/bin/sh";
  spec.args = {"-c", "echo hello_posix; exit 0"};
  spec.inherit_env = true;

  auto spawned = runner.Spawn(spec);
  assert(spawned);

  std::vector<std::string> lines;
  std::optional<int> code;
  for (int i = 0; i < 100 && !code.has_value(); ++i) {
    spawned.session->DrainOutput(lines);
    code = spawned.session->TryWait();
    if (!code.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  spawned.session->DrainOutput(lines);
  assert(code.has_value() && *code == 0);
  bool saw_hello = false;
  for (const auto& l : lines) {
    if (l == "hello_posix") saw_hello = true;
  }
  assert(saw_hello);

  // Verify Terminate cleanly kills a sleeping child process group without hanging
  SpawnSpec sleeper;
  sleeper.executable = "/bin/sh";
  sleeper.args = {"-c", "sleep 30"};
  auto s2 = runner.Spawn(sleeper);
  assert(s2);
  s2.session->Terminate(std::chrono::milliseconds(500));
  assert(s2.session->TryWait().has_value());

  std::cout << "PASS: TestPosixProcessRunnerSmoke\n";
}

// ============================================================================
// Test 8: Bloodborne Save Data Management (InspectSaves, Backups, Restore, Slot Recovery, 0x204E Audio Fix, Import)
// ============================================================================
void TestSaveDataManagementContracts() {
  TempWorkspace ws("saves");
  const fs::path saves_dir = ws.paths.data_dir / "user";
  const fs::path sprj = saves_dir / "savedata" / "1" / "CUSA00207" / "SPRJ0005";
  fs::create_directories(sprj);

  // Seed userdata0000 + backup0000, and userdata0010 + backup0010 with muted 0x204E (0)
  {
    std::vector<char> slot0(16384, 'A');
    std::ofstream(sprj / "userdata0000", std::ios::binary).write(slot0.data(), static_cast<std::streamsize>(slot0.size()));
    std::vector<char> bslot0(16384, 'B');
    std::ofstream(sprj / "backup0000", std::ios::binary).write(bslot0.data(), static_cast<std::streamsize>(bslot0.size()));

    std::vector<char> sys10(16384, 0);
    sys10[0x204E] = 0; // Muted sound bug state
    std::ofstream(sprj / "userdata0010", std::ios::binary).write(sys10.data(), static_cast<std::streamsize>(sys10.size()));
    std::ofstream(sprj / "backup0010", std::ios::binary).write(sys10.data(), static_cast<std::streamsize>(sys10.size()));
  }

  std::vector<SaveSlotItem> slots;
  std::vector<SaveBackupItem> backups;
  SaveSummary summary = InspectSaves(saves_dir, slots, backups);
  assert(summary.has_saves);
  assert(summary.active_title_id == "CUSA00207");
  assert(summary.character_slots == 1);
  assert(summary.system_slots == 1);
  assert(!summary.sound_flag_ok); // 0x204E is 0
  assert(slots.size() == 2);
  assert(slots[0].file_name == "userdata0000" && slots[0].has_internal_backup);
  assert(slots[1].file_name == "userdata0010" && slots[1].is_system_slot && !slots[1].sound_flag_ok);

  // Repair 0x204E sound flag
  const int patched = RepairSaveSoundFlags(saves_dir);
  assert(patched == 2);
  summary = InspectSaves(saves_dir, slots, backups);
  assert(summary.sound_flag_ok);
  assert(slots[1].sound_flag_ok);

  // Create a timestamped backup snapshot
  std::string backup_id;
  std::string err;
  assert(CreateSaveBackup(saves_dir, "before_boss", &backup_id, &err));
  assert(!backup_id.empty());
  summary = InspectSaves(saves_dir, slots, backups);
  assert(summary.backup_count == 1);
  assert(backups[0].id == backup_id);

  // Recover slot 0 from internal backup0000 ('B')
  assert(RestoreInternalSlotBackup(saves_dir, slots[0].id, &err));
  {
    std::ifstream in(sprj / "userdata0000", std::ios::binary);
    char first = 0;
    in.read(&first, 1);
    assert(first == 'B');
  }

  // Restore full snapshot ('A') and verify automatic pre_restore snapshot is created
  assert(RestoreSaveBackup(saves_dir, backup_id, &err));
  {
    std::ifstream in(sprj / "userdata0000", std::ios::binary);
    char first = 0;
    in.read(&first, 1);
    assert(first == 'A');
  }
  summary = InspectSaves(saves_dir, slots, backups);
  assert(summary.backup_count >= 2);

  // Delete backup snapshot
  assert(DeleteSaveBackup(saves_dir, backup_id, &err));
  summary = InspectSaves(saves_dir, slots, backups);
  for (const auto& b : backups) {
    assert(b.id != backup_id);
  }

  // Import external flat SPRJ0005 folder
  const fs::path ext_dir = ws.root / "external_save";
  fs::create_directories(ext_dir);
  {
    std::vector<char> ext_slot(16384, 'Z');
    std::ofstream(ext_dir / "userdata0001", std::ios::binary).write(ext_slot.data(), static_cast<std::streamsize>(ext_slot.size()));
  }
  assert(ImportExternalSaveFolder(saves_dir, ext_dir, "CUSA03173", &err));
  assert(fs::is_regular_file(saves_dir / "savedata" / "1" / "CUSA03173" / "SPRJ0005" / "userdata0001"));

  std::cout << "PASS: TestSaveDataManagementContracts\n";
}

} // namespace

int main() {
  std::cout << "=== Running Deepened Bloodborne Launcher Test Suite ===\n";
  TestSettingsStoreContract();
  TestLaunchEnvCompiler();
  TestGameDirectoryInspection();
  TestModsAndExternalPatchesContracts();
  TestTrophiesRealContract();
  TestLauncherCoreLifecycleAndSafety();
  TestPosixProcessRunnerSmoke();
  TestSaveDataManagementContracts();
  std::cout << "=== All 8 Deep Interface Test Suites Passed ===\n";
  return 0;
}

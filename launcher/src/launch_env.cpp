// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "launch_env.hpp"
#include "bbport_trophy_data.h"

#include <sstream>

namespace bbport {

namespace {

constexpr std::string_view kDefaultCommunityServer = "http://play.seamlesshunt.com";
constexpr std::string_view kDefaultShadnetServer = "play.seamlesshunt.com:31313";
constexpr std::string_view kDefaultHuntersDreamServer = "https://thehuntersdream.com";

std::string Trim(std::string_view sv) {
  std::size_t a = 0;
  while (a < sv.size() && (sv[a] == ' ' || sv[a] == '\t' || sv[a] == '\r' || sv[a] == '\n')) ++a;
  std::size_t b = sv.size();
  while (b > a && (sv[b - 1] == ' ' || sv[b - 1] == '\t' || sv[b - 1] == '\r' || sv[b - 1] == '\n')) --b;
  return std::string(sv.substr(a, b - a));
}

// Mirrors scripts/online.py::community_address()
std::string CommunityAddress(std::string_view value) {
  std::string v = Trim(value);
  if (v.empty()) v = std::string(kDefaultCommunityServer);
  std::string scheme;
  std::string rest = v;
  if (auto pos = v.rfind("://"); pos != std::string::npos) {
    scheme = v.substr(0, pos);
    rest = v.substr(pos + 3);
  }
  if (auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash);
  if (auto colon = rest.find(':'); colon != std::string::npos) rest = rest.substr(0, colon);

  const std::string default_scheme = "http";
  const std::string default_host = "play.seamlesshunt.com";
  if (scheme.empty()) {
    scheme = (rest == default_host) ? default_scheme : "https";
  }
  return scheme + "://" + rest;
}

// Mirrors scripts/online.py::webapi_address()
std::string WebApiAddress(std::string_view server) {
  std::string s = Trim(server);
  if (s.empty()) s = std::string(kDefaultShadnetServer);
  std::string host = s;
  auto first_colon = s.find(':');
  auto last_colon = s.rfind(':');
  if (first_colon != std::string::npos && first_colon == last_colon) {
    host = s.substr(0, first_colon);
  }
  return "http://" + host + ":31315";
}

// Mirrors scripts/online.py::webapi_setting()
std::string WebApiSetting(std::string_view webapi, std::string_view server, std::string_view community) {
  std::string w = Trim(webapi);
  const std::string comm = CommunityAddress(community);
  const auto sep = comm.find("://");
  const std::string comm_host = (sep != std::string::npos) ? comm.substr(sep + 3) : comm;
  if (!w.empty() && !comm_host.empty() && w.find(comm_host) != std::string::npos) {
    w.clear();
  }
  return w.empty() ? WebApiAddress(server) : w;
}

// Mirrors scripts/online.py::thd_base()
std::string ThdBase(std::string_view value) {
  std::string v = Trim(value);
  while (!v.empty() && v.back() == '/') v.pop_back();
  if (v.empty()) v = std::string(kDefaultHuntersDreamServer);
  if (v.find("://") == std::string::npos) {
    v = "https://" + v;
  }
  const auto sep = v.find("://");
  const std::string scheme = v.substr(0, sep);
  std::string rest = v.substr(sep + 3);
  if (auto slash = rest.find('/'); slash != std::string::npos) {
    rest = rest.substr(0, slash);
  }
  return scheme + "://" + rest;
}

} // namespace

LaunchPlan LaunchEnvCompiler::Compile(const PortPaths& paths,
                                      const SettingsStore& store,
                                      const SystemProbe& probe) {
  LaunchPlan plan;
  plan.executable = paths.RunnerScriptPath();
  plan.cwd = paths.port_dir;
  plan.env = probe.HostEnvironment();

  // 1. Canonical path & language exports (matching bbport_launcher_common.py::game_environment)
  const auto game_dir = store.ResolvedGameDir();
  if (!game_dir.empty()) {
    plan.env["BB_GAME_DIR"] = game_dir.string();
  }
  const std::string raw_user_dir = Trim(store.GetString("user_dir"));
  if (!raw_user_dir.empty()) {
    plan.env["BB_USER_DIR"] = store.ResolvedSavesDir().string();
  }
  plan.env["BB_MODS_DIR"] = store.ResolvedModsDir().string();
  plan.env["BB_MODS_CONFIG"] = paths.ModsConfigPath().string();
  plan.env["BB_PATCHES_DIR"] = store.ResolvedPatchesDir().string();
  plan.env["BB_PATCHES_CONFIG"] = paths.PatchesConfigPath().string();
  plan.env["BB_CONFIG"] = paths.IniPath().string();
  plan.env["BB_MENU_LANGUAGE"] = store.ResolvedUiLanguage(probe);

  // 2. Declarative 1:1 mappings from settings_schema.json ("env": {"var": ..., "mode": ...})
  for (const auto& def : store.Schema().Items()) {
    if (!def.env) {
      continue;
    }
    const auto& spec = *def.env;
    switch (spec.mode) {
      case EnvEmitMode::Always:
        plan.env[spec.var] = store.GetString(def.key);
        break;
      case EnvEmitMode::NonEmpty: {
        const std::string v = store.GetString(def.key);
        if (!v.empty()) {
          plan.env[spec.var] = v;
        }
        break;
      }
      case EnvEmitMode::ZeroOne:
        plan.env[spec.var] = store.GetBool(def.key) ? "1" : "0";
        break;
      case EnvEmitMode::TrueOnly:
        if (store.GetBool(def.key)) {
          plan.env[spec.var] = "1";
        }
        break;
      case EnvEmitMode::PositiveInt: {
        const int n = store.GetInt(def.key);
        if (n > 0) {
          plan.env[spec.var] = std::to_string(n);
        }
        break;
      }
    }
  }

  // 3. Multi-variable & conditional rules (explicit, tested C++ rules)

  // Rule A: MangoHud + default MANGOHUD_CONFIG (shell MANGOHUD_CONFIG wins)
  if (store.GetBool("mangohud")) {
    plan.env["MANGOHUD"] = "1";
    if (plan.env.find("MANGOHUD_CONFIG") == plan.env.end()) {
      plan.env["MANGOHUD_CONFIG"] = "fps,frametime,frame_timing,gpu_stats,cpu_stats,ram,vram";
    }
  }

  // Rule B: Selected GPU name -> Vulkan index BB_GPU_ID (shell BB_GPU_ID wins)
  const std::string chosen_gpu = store.GetString("gpu");
  if (!chosen_gpu.empty() && plan.env.find("BB_GPU_ID") == plan.env.end()) {
    const auto gpus = probe.ListGpus();
    bool matched = false;
    for (const auto& gpu : gpus) {
      if (gpu.name == chosen_gpu) {
        plan.env["BB_GPU_ID"] = std::to_string(gpu.vulkan_index);
        matched = true;
        break;
      }
    }
    if (!matched) {
      plan.warnings.push_back("Selected GPU not found in Vulkan list: " + chosen_gpu);
    }
  }

  // Rule C: Crash diagnostics -> 3 memory/write checks
  if (store.GetBool("crash_diag")) {
    plan.env["BB_FREE_CHECK"] = "1";
    plan.env["BB_WRITE_LOG"] = "1";
    plan.env["BB_PRODUCER_CHECK"] = "1";
  }

  // Rule D: Online multiplayer (seamless vs huntersdream, gated on online module presence)
  const std::string pool = (store.GetString("online_pool") == "huntersdream") ? "huntersdream" : "seamless";
  if (store.GetBool("online") && probe.HasOnlineModule(paths, pool)) {
    std::string server = Trim(store.GetString("online_server"));
    if (server.empty()) server = std::string(kDefaultShadnetServer);
    const std::string community = CommunityAddress(store.GetString("online_community"));

    plan.env["BB_ONLINE"] = "1";
    // Online needs the game's online / offline choice: the skip patch opens the menu offline, so
    // the player signs in to shadNet but never reaches the game server.
    plan.env["BB_SKIP_NETWORK_CHOICE"] = "0";
    plan.env["BB_SHADNET_SERVER"] = server;
    plan.env["BB_SHADNET_WEBAPI"] = WebApiSetting(store.GetString("online_webapi"), server, community);
    plan.env["BB_SHADNET_NPID"] = Trim(store.GetString("online_npid"));
    plan.env["BB_SHADNET_PASSWORD"] = store.GetString("online_password");
    plan.env["BB_UPNP"] = store.GetBool("online_upnp") ? "1" : "0";
    plan.env["BB_COMMUNITY_SERVER"] = community;

    if (pool == "seamless" && store.GetBool("online_bbsc")) {
      plan.env["BB_BBSC"] = "1";
    }
    if (pool == "huntersdream") {
      plan.env["BB_NET_BACKEND"] = "thd";
      plan.env["BB_COMMUNITY_SERVER"] = ThdBase(store.GetString("thd_server"));
      plan.env["BB_THD_ONLINE_ID"] = store.GetString("thd_online_id");
      plan.env["BB_THD_TOKEN"] = store.GetString("thd_token");
    }
  }

  // Rule E: Trophy key validation via shared BbTrophy::ParseKey (32 hex digits)
  const std::string trophy_key = Trim(store.GetString("trophy_key"));
  if (!trophy_key.empty() && BbTrophy::ParseKey(trophy_key).has_value()) {
    plan.env["BB_TROPHY_KEY"] = trophy_key;
  }

  // Rule F: User extra_env whitespace-delimited KEY=VALUE overrides (applied last)
  std::istringstream extra(store.GetString("extra_env"));
  std::string token;
  while (extra >> token) {
    const auto eq = token.find('=');
    if (eq != std::string::npos && eq > 0) {
      plan.env[token.substr(0, eq)] = token.substr(eq + 1);
    }
  }

  return plan;
}

} // namespace bbport

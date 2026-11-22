// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_paths.hpp"

#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

namespace bbport {

namespace {

bool LooksLikePortDir(const fs::path& dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / "launcher" / "settings_schema.json", ec) ||
         fs::is_regular_file(dir / "run.sh", ec) ||
         fs::is_regular_file(dir / "run.py", ec) ||
         fs::is_regular_file(dir / "bbport-run.exe", ec);
}

fs::path ResolvePortDir(const std::optional<fs::path>& executable_path) {
  std::vector<fs::path> seeds;
  if (executable_path && !executable_path->empty()) {
    std::error_code ec;
    auto abs = fs::weakly_canonical(*executable_path, ec);
    if (!ec && !abs.empty()) {
      seeds.push_back(fs::is_directory(abs, ec) ? abs : abs.parent_path());
    }
  }
  {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
      seeds.push_back(cwd);
    }
  }

  for (const auto& seed : seeds) {
    fs::path cur = seed;
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
      if (LooksLikePortDir(cur)) {
        return cur;
      }
      auto parent = cur.parent_path();
      if (parent == cur) {
        break;
      }
      cur = parent;
    }
  }

  std::error_code ec;
  auto cwd = fs::current_path(ec);
  return ec ? fs::path(".") : cwd;
}

} // namespace

fs::path PortPaths::SchemaPath() const {
  return port_dir / "launcher" / "settings_schema.json";
}

fs::path PortPaths::SettingsJsonPath() const {
  return config_dir / "settings.json";
}

fs::path PortPaths::IniPath() const {
  if (const char* override_ini = std::getenv("BB_CONFIG"); override_ini && override_ini[0] != '\0') {
    return fs::path(override_ini);
  }
  return data_dir / "bbport.ini";
}

fs::path PortPaths::ModsConfigPath() const {
  return data_dir / "mods.json";
}

fs::path PortPaths::PatchesConfigPath() const {
  return data_dir / "patches.json";
}

fs::path PortPaths::BuiltinPatchesXmlPath() const {
  return port_dir / "patches" / "Bloodborne.xml";
}

fs::path PortPaths::RunnerScriptPath() const {
#ifdef _WIN32
  std::error_code ec;
  if (fs::is_regular_file(port_dir / "bbport-run.exe", ec)) {
    return port_dir / "bbport-run.exe";
  }
  if (fs::is_regular_file(port_dir / "run.py", ec)) {
    return port_dir / "run.py";
  }
#endif
  return port_dir / "run.sh";
}

PortPaths DiscoverPortPaths(std::optional<fs::path> executable_path) {
  PortPaths paths;
  paths.port_dir = ResolvePortDir(executable_path);
  if (const char* prebuilt = std::getenv("BB_PREBUILT"); prebuilt && std::string_view(prebuilt) == "1") {
    paths.packaged = true;
  }

  if (const char* bb_data = std::getenv("BB_DATA_DIR"); bb_data && bb_data[0] != '\0') {
    paths.data_dir = ExpandTilde(bb_data);
  } else {
    paths.data_dir = paths.port_dir;
  }

  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
    paths.config_dir = fs::path(xdg) / "bbport-launcher";
  } else if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
    paths.config_dir = fs::path(home) / ".config" / "bbport-launcher";
  } else {
    paths.config_dir = paths.data_dir / ".config" / "bbport-launcher";
  }

  return paths;
}

fs::path ExpandTilde(std::string_view raw_path) {
  if (raw_path.empty()) {
    return {};
  }
  if (raw_path[0] == '~' && (raw_path.size() == 1 || raw_path[1] == '/')) {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
      if (raw_path.size() == 1) {
        return fs::path(home);
      }
      return fs::path(home) / std::string(raw_path.substr(2));
    }
  }
  return fs::path(std::string(raw_path));
}

} // namespace bbport

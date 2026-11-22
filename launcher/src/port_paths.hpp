// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bbport {

// Local-substitutable directory roots (see GLOSSARY.md).
struct PortPaths {
  std::filesystem::path port_dir;   // Port Directory: run.sh, launcher/settings_schema.json, tools/
  std::filesystem::path data_dir;   // Data Directory: bbport.ini, mods.json, patches.json, default mods/patches/saves
  std::filesystem::path config_dir; // Config Directory: settings.json (0600)
  bool packaged = false;

  std::filesystem::path SchemaPath() const;
  std::filesystem::path SettingsJsonPath() const;
  std::filesystem::path IniPath() const;
  std::filesystem::path ModsConfigPath() const;
  std::filesystem::path PatchesConfigPath() const;
  std::filesystem::path BuiltinPatchesXmlPath() const;
  std::filesystem::path RunnerScriptPath() const;
};

// Discovers production Linux directories from environment and executable/CWD location.
PortPaths DiscoverPortPaths(std::optional<std::filesystem::path> executable_path = std::nullopt);

// Expands a leading "~/" using HOME when present.
std::filesystem::path ExpandTilde(std::string_view raw_path);

} // namespace bbport

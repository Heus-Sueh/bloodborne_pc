// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "port_paths.hpp"
#include "settings_store.hpp"
#include "system_probe.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bbport {

struct LaunchPlan {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::filesystem::path cwd;
  std::map<std::string, std::string> env;
  std::vector<std::string> warnings;
};

// Compiles schema-declared 1:1 BB_* mappings plus conditional/multi-variable launch rules.
class LaunchEnvCompiler {
public:
  static LaunchPlan Compile(const PortPaths& paths, const SettingsStore& store, const SystemProbe& probe);
};

} // namespace bbport

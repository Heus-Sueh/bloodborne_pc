// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "system_probe.hpp"
#include "gpu_capabilities.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace bbport {

LinuxSystemProbe::LinuxSystemProbe(PortPaths paths) : paths_(std::move(paths)) {}

std::vector<GpuInfo> LinuxSystemProbe::ListGpus() const {
  return ProbeVulkanGpusInProcess();
}

bool LinuxSystemProbe::HasOnlineModule(const PortPaths& paths, std::string_view /*pool*/) const {
  std::error_code ec;
  if (const char* forced = std::getenv("BB_NET_MODULE"); forced && forced[0] != '\0') {
    return fs::is_regular_file(fs::path(forced), ec);
  }
#ifdef _WIN32
  if (fs::is_regular_file(paths.port_dir / "online" / "bbnet20.dll", ec) ||
      fs::is_regular_file(paths.port_dir / "out" / "gpu" / "bbnet20.dll", ec)) {
    return true;
  }
#endif
  const fs::path candidate = paths.port_dir / (paths.packaged ? "bin" : "out") / "gpu" / "libbbnet20.so";
  return fs::is_regular_file(candidate, ec);
}

std::map<std::string, std::string> LinuxSystemProbe::HostEnvironment() const {
  std::map<std::string, std::string> env;
#ifdef _WIN32
  LPCH strings = ::GetEnvironmentStringsA();
  if (!strings) return env;
  for (LPCH p = strings; *p != '\0'; p += std::strlen(p) + 1) {
    std::string_view entry(p);
    auto eq = entry.find('=');
    if (eq != std::string_view::npos && eq > 0) {
      env.emplace(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
  }
  ::FreeEnvironmentStringsA(strings);
  return env;
#else
  if (!environ) {
    return env;
  }
  for (char** p = environ; *p != nullptr; ++p) {
    std::string_view entry(*p);
    auto eq = entry.find('=');
    if (eq != std::string_view::npos && eq > 0) {
      env.emplace(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
  }
  return env;
#endif
}

std::string LinuxSystemProbe::DetectSystemLanguage() const {
  const char* raw = std::getenv("LC_ALL");
  if (!raw || raw[0] == '\0') {
    raw = std::getenv("LANG");
  }
  if (!raw) {
    return "en";
  }
  std::string_view lang(raw);
  if (lang.rfind("pt", 0) == 0) return "pt_BR";
  if (lang.rfind("ru", 0) == 0) return "ru";
  if (lang.rfind("zh", 0) == 0) return "zh_CN";
  if (lang.rfind("fr", 0) == 0) return "fr_FR";
  return "en";
}

} // namespace bbport

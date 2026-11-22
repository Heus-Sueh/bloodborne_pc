// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "system_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace bbport {

namespace {

std::string RunQuietTool(const fs::path& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
  std::string cmd = "\"" + exe.string() + "\"";
  for (const auto& a : args) {
    cmd += " \"" + a + "\"";
  }
  FILE* pipe = ::_popen(cmd.c_str(), "r");
  if (!pipe) return {};
  std::string out;
  char buf[512];
  while (std::size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
    out.append(buf, n);
  }
  if (::_pclose(pipe) != 0) return {};
  return out;
#else
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    return {};
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return {};
  }
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> argv;
    std::string exe_str = exe.string();
    argv.push_back(exe_str.data());
    std::vector<std::string> copy = args;
    for (auto& a : copy) {
      argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    ::execv(exe_str.c_str(), argv.data());
    ::_exit(127);
  }

  ::close(pipefd[1]);
  std::string out;
  char buf[512];
  ssize_t n = 0;
  while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return {};
  }
  return out;
#endif
}

} // namespace

LinuxSystemProbe::LinuxSystemProbe(PortPaths paths) : paths_(std::move(paths)) {}

std::vector<GpuInfo> LinuxSystemProbe::ListGpus() const {
  std::error_code ec;
#ifdef _WIN32
  const char* tool_name = "bb-gpu-capabilities.exe";
#else
  const char* tool_name = "bb-gpu-capabilities";
#endif
  fs::path tool = paths_.port_dir / (paths_.packaged ? "bin" : "out") / tool_name;
  if (!fs::is_regular_file(tool, ec)) {
    tool = paths_.port_dir / "bin" / tool_name;
  }
  if (!fs::is_regular_file(tool, ec)) {
    tool = paths_.port_dir / "out" / tool_name;
    if (!fs::is_regular_file(tool, ec)) {
      return {};
    }
  }

  const std::string output = RunQuietTool(tool, {"--gpus"});
  if (output.empty()) {
    return {};
  }

  std::vector<GpuInfo> gpus;
  std::istringstream iss(output);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) {
      continue;
    }
    // Format from tools/gpu_capabilities.c: index\tvendor\tkind\tname[\tbest]
    std::vector<std::string> cols;
    std::size_t start = 0;
    while (start <= line.size()) {
      auto tab = line.find('\t', start);
      if (tab == std::string::npos) {
        cols.push_back(line.substr(start));
        break;
      }
      cols.push_back(line.substr(start, tab - start));
      start = tab + 1;
    }
    if (cols.size() >= 4) {
      try {
        GpuInfo info;
        info.vulkan_index = static_cast<std::uint32_t>(std::stoul(cols[0]));
        info.name = cols[3];
        info.preferred = (cols.size() >= 5 && (cols[4] == "1" || cols[4] == "best" || cols[4] == "true"));
        gpus.push_back(std::move(info));
      } catch (...) {
      }
    }
  }
  return gpus;
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

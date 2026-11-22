// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bbport {

struct SpawnSpec {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::filesystem::path cwd;
  std::map<std::string, std::string> env;
  bool inherit_env = true;
};

// RAII handle for a running child process; destructor guarantees process-group termination and reap.
class ProcessSession {
public:
  virtual ~ProcessSession() = default;
  virtual void DrainOutput(std::vector<std::string>& out_lines) = 0;
  virtual std::optional<int> TryWait() = 0;
  virtual void Terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(1500)) = 0;
};

struct SpawnResult {
  std::unique_ptr<ProcessSession> session;
  std::string error;
  explicit operator bool() const noexcept { return session != nullptr; }
};

// Seam for spawning child processes (run.sh, extractors).
// Two adapters: PosixProcessRunner (production) and FakeProcessRunner (tests).
class ProcessRunner {
public:
  virtual ~ProcessRunner() = default;
  virtual SpawnResult Spawn(const SpawnSpec& spec) = 0;
};

class PosixProcessRunner final : public ProcessRunner {
public:
  SpawnResult Spawn(const SpawnSpec& spec) override;
};

} // namespace bbport

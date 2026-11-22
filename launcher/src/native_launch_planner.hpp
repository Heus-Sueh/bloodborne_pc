// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bbport {

inline constexpr int kRestartExitCode = 75;

struct NativeExecutionPlan {
  std::filesystem::path game_dir;
  std::filesystem::path original_game;
  std::optional<std::filesystem::path> mod_game;
  std::filesystem::path out_dir;
  std::filesystem::path user_dir;
  std::filesystem::path probe_executable;
  std::vector<std::string> probe_args;
  std::map<std::string, std::string> env_set;
  std::vector<std::string> env_unset;
  std::map<std::string, std::string> full_env;
};

class NativeLaunchPlanner {
public:
  using LogCallback = std::function<void(const std::string&)>;

  /// Runs all pre-launch preparation steps natively in C++ (zero Python required):
  /// 1. Mods discovery, save backup, and symlink/hardlink overlay (`out/mod-game-*`)
  /// 2. PS4 SELF/ELF extraction (`out/eboot.elf`, `out/boot.bin`) and module linking
  ///    (`libc.prx`, `libSceFios2.prx` -> `out/boot-linked.bin` in BBPROBE5 format)
  /// 3. AppContent SFO profile (`out/content.bin` in BBCONT01 format)
  /// 4. Monitor resolution detection (`bb-gpu-capabilities --displays`), hybrid preset sizing,
  ///    and XML patch compilation (`patches/Bloodborne.xml` + external XMLs -> `out/patches.bin`)
  /// 5. Online module resolution and `host_overrides.json` generation
  static bool PlanAndPrepare(
      const std::map<std::string, std::string>& base_env,
      const std::filesystem::path& port_dir,
      const std::filesystem::path& data_dir,
      const std::filesystem::path& caps_path,
      bool skip_prepare,
      bool skip_online,
      NativeExecutionPlan& out_plan,
      std::string* out_error = nullptr,
      LogCallback log = nullptr);

  /// Writes the NUL-delimited plan file (`KIND\0KEY\0VAL\0`) for `run.sh` compatibility.
  static bool WriteNulPlanFile(const NativeExecutionPlan& plan,
                               const std::filesystem::path& dest,
                               std::string* out_error = nullptr);
};

} // namespace bbport

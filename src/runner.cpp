#include "config_manager.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static void SetEnvVar(const std::string& name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

static std::string GetEnvVar(const std::string& name, const std::string& fallback = "") {
  const char* val = std::getenv(name.c_str());
  return (val && val[0]) ? std::string(val) : fallback;
}

static std::string FindPython() {
  std::string py = GetEnvVar("PYTHON");
  if (!py.empty() && fs::exists(py)) {
    return py;
  }
#ifdef _WIN32
  std::string msys = GetEnvVar("BB_MSYS2", "C:\\msys64");
  std::string msys_py = msys + "\\clang64\\bin\\python.exe";
  if (fs::exists(msys_py)) {
    return msys_py;
  }
  return "python.exe";
#else
  for (const char* candidate : {"python3", "python"}) {
    char path[1024];
    std::string cmd = std::string("command -v ") + candidate + " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      if (fgets(path, sizeof(path), fp)) {
        std::string p = path;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
          p.pop_back();
        pclose(fp);
        if (!p.empty() && fs::exists(p))
          return p;
      } else {
        pclose(fp);
      }
    }
  }
  return "python3";
#endif
}

static int RunCommand(const std::vector<std::string>& args, std::string* out_output = nullptr) {
  if (args.empty())
    return -1;
#ifdef _WIN32
  std::string cmdline;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0)
      cmdline += " ";
    if (args[i].find(' ') != std::string::npos) {
      cmdline += "\"" + args[i] + "\"";
    } else {
      cmdline += args[i];
    }
  }
  if (out_output) {
    FILE* pipe = _popen(cmdline.c_str(), "r");
    if (!pipe)
      return -1;
    char buffer[256];
    out_output->clear();
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      *out_output += buffer;
    }
    return _pclose(pipe);
  } else {
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmdline.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
  }
#else
  if (out_output) {
    std::string cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0)
        cmdline += " ";
      cmdline += "'" + args[i] + "'";
    }
    FILE* pipe = popen(cmdline.c_str(), "r");
    if (!pipe)
      return -1;
    char buffer[256];
    out_output->clear();
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      *out_output += buffer;
    }
    int status = pclose(pipe);
    if (WIFEXITED(status))
      return WEXITSTATUS(status);
    return status;
  } else {
    pid_t pid = fork();
    if (pid == 0) {
      std::vector<char*> c_args;
      for (const auto& a : args)
        c_args.push_back(const_cast<char*>(a.c_str()));
      c_args.push_back(nullptr);
      execvp(c_args[0], c_args.data());
      std::perror("execvp failed");
      _exit(127);
    } else if (pid > 0) {
      int status = 0;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status))
        return WEXITSTATUS(status);
      return status;
    }
    return -1;
  }
#endif
}

int main(int argc, char* argv[]) {
  LauncherSettings cfg = ConfigManager::Load();

  std::string render_res;
  std::string live_res = "0";
  std::string mods_enabled = "1";
  bool build_only = false;
  std::vector<std::string> extra_args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--game-dir" && i + 1 < argc) {
      cfg.game_dir = argv[++i];
    } else if (arg == "--user" && i + 1 < argc) {
      cfg.user_dir = argv[++i];
    } else if (arg == "--mods-dir" && i + 1 < argc) {
      cfg.mods_dir = argv[++i];
    } else if (arg == "--fps" && i + 1 < argc) {
      std::string val = argv[++i];
      if (val == "uncap" || val == "0")
        cfg.fps_limit = 0;
      else
        cfg.fps_limit = std::atoi(val.c_str());
    } else if (arg == "--fps-limit" && i + 1 < argc) {
      cfg.fps_limit = std::atoi(argv[++i]);
    } else if (arg == "--fullscreen") {
      cfg.fullscreen = true;
    } else if (arg == "--borderless") {
      cfg.borderless = true;
    } else if (arg == "--vsync") {
      cfg.vsync = true;
    } else if (arg == "--no-vsync") {
      cfg.vsync = false;
    } else if (arg == "--render-res" && i + 1 < argc) {
      render_res = argv[++i];
    } else if (arg == "--output-res" && i + 1 < argc) {
      cfg.output_res = argv[++i];
    } else if (arg == "--live-res" && i + 1 < argc) {
      live_res = argv[++i];
    } else if (arg == "--build-only") {
      build_only = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Bloodborne PC Native Runner (C++23)\n"
                << "Usage: bb-runner [options] [probe options...]\n\n"
                << "Options:\n"
                << "  --game-dir <DIR>    Set game dump directory (CUSA03173)\n"
                << "  --user <DIR>        Set user save directory (default: user)\n"
                << "  --mods-dir <DIR>    Set mods directory (default: mods)\n"
                << "  --fullscreen        Launch in fullscreen mode\n"
                << "  --borderless        Launch as borderless window\n"
                << "  --vsync / --no-vsync Toggle vertical synchronization\n"
                << "  --fps <RATE>        Set frame rate (uncap, 60, 90, 30)\n"
                << "  --fps-limit <FPS>   Set numeric frame rate limit\n"
                << "  --render-res <WxH>  Set internal render resolution\n"
                << "  --output-res <WxH>  Set window/display output resolution\n"
                << "  --live-res <0|1>    Toggle live resolution scaling\n"
                << "  --build-only        Build binaries without starting the game\n";
      return 0;
    } else {
      extra_args.push_back(arg);
    }
  }

  std::string data_dir = GetEnvVar("BB_DATA_DIR", ".");
  fs::path out_dir = fs::path(data_dir) / "out";
  fs::create_directories(out_dir);

  if (cfg.game_dir.empty()) {
    cfg.game_dir = GetEnvVar("BB_GAME_DIR");
  }
  fs::path remembered_file = out_dir / "game_dir.txt";
  if (cfg.game_dir.empty() && fs::exists(remembered_file)) {
    std::ifstream rf(remembered_file);
    std::getline(rf, cfg.game_dir);
  }
  if (cfg.game_dir.empty()) {
    if (fs::exists("../CUSA03173/eboot.bin")) {
      cfg.game_dir = "../CUSA03173";
    } else if (fs::exists("CUSA03173/eboot.bin")) {
      cfg.game_dir = "CUSA03173";
    }
  }

  if (build_only) {
    std::cout << "[RUNNER] Building binaries via scripts/build.sh...\n";
    return RunCommand({"bash", "scripts/build.sh"});
  }

  if (cfg.game_dir.empty() || !fs::exists(fs::path(cfg.game_dir) / "eboot.bin")) {
    std::cerr << "ERROR: No eboot.bin found in '" << cfg.game_dir
              << "'. Set BB_GAME_DIR or pass --game-dir.\n";
    return 1;
  }

  fs::path game_abs = fs::canonical(cfg.game_dir);
  {
    std::ofstream rf(remembered_file);
    rf << game_abs.string() << "\n";
  }

  // Unified configuration environment application
  ConfigManager::ApplyToEnvironment(cfg);
  SetEnvVar("BB_DATA_DIR", data_dir);
  SetEnvVar("BB_MODS_ENABLED", mods_enabled);
  if (!render_res.empty())
    SetEnvVar("BB_RENDER_RES", render_res);
  if (!live_res.empty())
    SetEnvVar("BB_LIVE_RES", live_res);

  std::string python = FindPython();

  std::cout << "[RUNNER] Initializing game preparation for " << game_abs << "...\n";

  // 1. Resolve active game path (Native VFS handles mod layering in-memory)
  std::string active_game = game_abs.string();

  // 2. Prepare ELF / SELF structure
  std::cout << "[RUNNER] Inspecting eboot.bin (prepare.py)...\n";
  if (RunCommand({python, "scripts/prepare.py", active_game, "--out", out_dir.string()}) != 0) {
    std::cerr << "ERROR: Failed to prepare eboot image.\n";
    return 1;
  }

  // 3. Link modules (libc.prx + libSceFios2.prx) -> boot-linked.bin (BBPROBE5)
  std::cout << "[RUNNER] Linking system modules (link_modules.py)...\n";
  if (RunCommand({python, "scripts/link_modules.py", active_game, "--out", out_dir.string()}) !=
      0) {
    std::cerr << "ERROR: Failed to link system modules.\n";
    return 1;
  }

  // 4. Generate content profile
  RunCommand({python,
              "scripts/content_profile.py",
              active_game,
              "--out",
              out_dir.string(),
              "--sku",
              "full"});

  // 5. Compile patches
  std::string config_path = ConfigManager::GetConfigPath();
  std::string fps_arg = (cfg.fps_limit > 0 ? std::to_string(cfg.fps_limit) : "uncap");
  std::vector<std::string> patch_cmd = {python,
                                        "scripts/patches.py",
                                        "--out",
                                        out_dir.string(),
                                        "--fps",
                                        fps_arg,
                                        "--game-dir",
                                        active_game,
                                        "--settings",
                                        config_path};
  if (!render_res.empty()) {
    patch_cmd.push_back("--render-res");
    patch_cmd.push_back(render_res);
  }
  if (!cfg.output_res.empty()) {
    patch_cmd.push_back("--output-res");
    patch_cmd.push_back(cfg.output_res);
  }
  RunCommand(patch_cmd);

  // 6. Ensure probe executable is compiled
  fs::path probe = out_dir / "bb-probe";
#ifdef _WIN32
  probe += ".exe";
#endif
  if (!fs::exists(probe)) {
    std::cout << "[RUNNER] " << probe.string() << " not found, building...\n";
    RunCommand({"bash", "scripts/build.sh"});
  }

  if (!fs::exists(probe)) {
    std::cerr << "ERROR: Probe executable could not be found or built: " << probe.string() << "\n";
    return 1;
  }

  // 7. Launch execution layer (watchdog disabled via --timeout 0)
  std::string timeout = GetEnvVar("BB_TIMEOUT", "0");
  std::vector<std::string> probe_cmd = {probe.string(),
                                        (out_dir / "boot-linked.bin").string(),
                                        "--content-profile",
                                        (out_dir / "content.bin").string(),
                                        "--patches",
                                        (out_dir / "patches.bin").string(),
                                        "--app0",
                                        active_game,
                                        "--user",
                                        cfg.user_dir,
                                        "--timeout",
                                        timeout};
  for (const auto& a : extra_args) {
    probe_cmd.push_back(a);
  }

  std::cout << "[RUNNER] Launching Bloodborne execution layer:\n  " << probe.string() << " "
            << (out_dir / "boot-linked.bin").string() << "\n";
  return RunCommand(probe_cmd);
}

// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "process_runner.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace bbport {

#ifdef _WIN32

namespace {

class Win32ProcessSession final : public ProcessSession {
public:
  Win32ProcessSession(HANDLE proc, HANDLE read_pipe) : proc_(proc), read_pipe_(read_pipe) {
    reader_thread_ = std::thread([this]() {
      char buf[1024];
      std::string current;
      DWORD n = 0;
      while (::ReadFile(read_pipe_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
          if (buf[i] == '\n') {
            PushLine(current);
            current.clear();
          } else if (buf[i] != '\r') {
            current.push_back(buf[i]);
          }
        }
      }
      if (!current.empty()) {
        PushLine(current);
      }
      ::CloseHandle(read_pipe_);
      read_pipe_ = INVALID_HANDLE_VALUE;
    });
  }

  ~Win32ProcessSession() override {
    Terminate(std::chrono::milliseconds(500));
    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }
    if (proc_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(proc_);
      proc_ = INVALID_HANDLE_VALUE;
    }
  }

  void DrainOutput(std::vector<std::string>& out_lines) override {
    std::lock_guard<std::mutex> lock(lines_mutex_);
    if (pending_lines_.empty()) return;
    out_lines.insert(out_lines.end(),
                     std::make_move_iterator(pending_lines_.begin()),
                     std::make_move_iterator(pending_lines_.end()));
    pending_lines_.clear();
  }

  std::optional<int> TryWait() override {
    if (exit_code_.has_value()) return exit_code_;
    if (proc_ == INVALID_HANDLE_VALUE) return exit_code_;
    if (::WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
      DWORD code = 0;
      ::GetExitCodeProcess(proc_, &code);
      exit_code_ = static_cast<int>(code);
      ::CloseHandle(proc_);
      proc_ = INVALID_HANDLE_VALUE;
      if (reader_thread_.joinable()) {
        reader_thread_.join();
      }
    }
    return exit_code_;
  }

  void Terminate(std::chrono::milliseconds /*grace*/) override {
    if (proc_ != INVALID_HANDLE_VALUE) {
      ::TerminateProcess(proc_, 1);
      ::WaitForSingleObject(proc_, 500);
      DWORD code = 1;
      ::GetExitCodeProcess(proc_, &code);
      exit_code_ = static_cast<int>(code);
      ::CloseHandle(proc_);
      proc_ = INVALID_HANDLE_VALUE;
    }
    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }
  }

private:
  void PushLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex_);
    pending_lines_.push_back(line);
  }

  HANDLE proc_ = INVALID_HANDLE_VALUE;
  HANDLE read_pipe_ = INVALID_HANDLE_VALUE;
  std::optional<int> exit_code_;
  std::mutex lines_mutex_;
  std::vector<std::string> pending_lines_;
  std::thread reader_thread_;
};

std::string QuoteWinArg(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

} // namespace

SpawnResult PosixProcessRunner::Spawn(const SpawnSpec& spec) {
  if (spec.executable.empty()) {
    return {nullptr, "Empty executable path"};
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = INVALID_HANDLE_VALUE;
  HANDLE write_pipe = INVALID_HANDLE_VALUE;
  if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    return {nullptr, "CreatePipe failed"};
  }
  ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  std::string cmdline = QuoteWinArg(spec.executable.string());
  for (const auto& a : spec.args) {
    cmdline.push_back(' ');
    cmdline += QuoteWinArg(a);
  }

  for (const auto& [k, v] : spec.env) {
    ::SetEnvironmentVariableA(k.c_str(), v.c_str());
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  const std::string cwd_str = spec.cwd.string();
  const BOOL ok = ::CreateProcessA(
      nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, cwd_str.empty() ? nullptr : cwd_str.c_str(), &si, &pi);
  ::CloseHandle(write_pipe);
  if (!ok) {
    ::CloseHandle(read_pipe);
    return {nullptr, "CreateProcessA failed"};
  }
  ::CloseHandle(pi.hThread);
  return {std::make_unique<Win32ProcessSession>(pi.hProcess, read_pipe), {}};
}

#else

namespace {

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  ~UniqueFd() noexcept { Reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  int Get() const noexcept { return fd_; }
  int Release() noexcept {
    int tmp = fd_;
    fd_ = -1;
    return tmp;
  }
  void Reset(int new_fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

int DecodeWaitStatus(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

class PosixProcessSession final : public ProcessSession {
public:
  PosixProcessSession(pid_t pid, UniqueFd read_fd) : pid_(pid) {
    reader_thread_ = std::thread([this, fd = std::move(read_fd)]() mutable {
      char buf[1024];
      std::string current;
      while (true) {
        ssize_t n = ::read(fd.Get(), buf, sizeof(buf));
        if (n < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (n == 0) {
          break;
        }
        for (ssize_t i = 0; i < n; ++i) {
          if (buf[i] == '\n') {
            PushLine(current);
            current.clear();
          } else if (buf[i] != '\r') {
            current.push_back(buf[i]);
          }
        }
      }
      if (!current.empty()) {
        PushLine(current);
      }
    });
  }

  ~PosixProcessSession() override {
    Terminate(std::chrono::milliseconds(500));
    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }
  }

  void DrainOutput(std::vector<std::string>& out_lines) override {
    std::lock_guard<std::mutex> lock(lines_mutex_);
    if (pending_lines_.empty()) {
      return;
    }
    out_lines.insert(out_lines.end(),
                     std::make_move_iterator(pending_lines_.begin()),
                     std::make_move_iterator(pending_lines_.end()));
    pending_lines_.clear();
  }

  std::optional<int> TryWait() override {
    if (exit_code_.has_value()) {
      return exit_code_;
    }
    if (pid_ <= 0) {
      return exit_code_;
    }
    int status = 0;
    const pid_t res = ::waitpid(pid_, &status, WNOHANG);
    if (res > 0) {
      exit_code_ = DecodeWaitStatus(status);
      pid_ = -1;
      if (reader_thread_.joinable()) {
        reader_thread_.join();
      }
    }
    return exit_code_;
  }

  void Terminate(std::chrono::milliseconds grace) override {
    if (pid_ <= 0) {
      if (reader_thread_.joinable()) {
        reader_thread_.join();
      }
      return;
    }

    // Signal the whole session process group created by setsid().
    ::killpg(pid_, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t res = ::waitpid(pid_, &status, WNOHANG);
      if (res > 0) {
        exit_code_ = DecodeWaitStatus(status);
        pid_ = -1;
        break;
      }
      if (res < 0 && errno == ECHILD) {
        pid_ = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    if (pid_ > 0) {
      ::killpg(pid_, SIGKILL);
      int status = 0;
      if (::waitpid(pid_, &status, 0) > 0) {
        exit_code_ = DecodeWaitStatus(status);
      }
      pid_ = -1;
    }

    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }
  }

private:
  void PushLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex_);
    pending_lines_.push_back(line);
  }

  pid_t pid_ = -1;
  std::optional<int> exit_code_;
  std::mutex lines_mutex_;
  std::vector<std::string> pending_lines_;
  std::thread reader_thread_;
};

} // namespace

SpawnResult PosixProcessRunner::Spawn(const SpawnSpec& spec) {
  if (spec.executable.empty()) {
    return {nullptr, "Empty executable path"};
  }

  int raw_fds[2] = {-1, -1};
#ifdef __linux__
  if (::pipe2(raw_fds, O_CLOEXEC) != 0) {
    return {nullptr, std::string("pipe2 failed: ") + std::strerror(errno)};
  }
#else
  if (::pipe(raw_fds) != 0) {
    return {nullptr, std::string("pipe failed: ") + std::strerror(errno)};
  }
  ::fcntl(raw_fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(raw_fds[1], F_SETFD, FD_CLOEXEC);
#endif

  UniqueFd read_fd(raw_fds[0]);
  UniqueFd write_fd(raw_fds[1]);

  // Prepare argv and envp before fork() to stay async-signal-safe in child.
  std::vector<std::string> argv_storage;
  argv_storage.reserve(1 + spec.args.size());
  argv_storage.push_back(spec.executable.string());
  for (const auto& a : spec.args) {
    argv_storage.push_back(a);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto& a : argv_storage) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);

  std::map<std::string, std::string> merged_env;
  if (spec.inherit_env && environ) {
    for (char** p = environ; *p != nullptr; ++p) {
      std::string_view entry(*p);
      auto eq = entry.find('=');
      if (eq != std::string_view::npos && eq > 0) {
        merged_env[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
      }
    }
  }
  for (const auto& [k, v] : spec.env) {
    merged_env[k] = v;
  }

  std::vector<std::string> env_storage;
  env_storage.reserve(merged_env.size());
  for (const auto& [k, v] : merged_env) {
    env_storage.push_back(k + "=" + v);
  }
  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto& e : env_storage) {
    envp.push_back(e.data());
  }
  envp.push_back(nullptr);

  const std::string cwd_str = spec.cwd.string();
  const std::string exe_str = spec.executable.string();

  const pid_t pid = ::fork();
  if (pid < 0) {
    return {nullptr, std::string("fork failed: ") + std::strerror(errno)};
  }

  if (pid == 0) {
    ::setsid();
    ::dup2(write_fd.Get(), STDOUT_FILENO);
    ::dup2(write_fd.Get(), STDERR_FILENO);
    read_fd.Reset();
    write_fd.Reset();

    if (!cwd_str.empty()) {
      (void)::chdir(cwd_str.c_str());
    }

    ::execve(exe_str.c_str(), argv.data(), envp.data());
    ::_exit(127);
  }

  write_fd.Reset();
  return {std::make_unique<PosixProcessSession>(pid, std::move(read_fd)), {}};
}

#endif

} // namespace bbport

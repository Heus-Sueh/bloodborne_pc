---
name: cpp-coding-standards
description: >-
  C++ & C coding standards for modern C++20/C++23 subsystems and C11 low-level runtimes:
  resource management (RAII), handle ownership, concurrency & thread synchronization,
  clean dialect boundaries, and static analysis tooling.
  Use when writing, refactoring, or auditing C++ or C code.
---

# C++ & C Coding Standards

Definitive guidelines for clean, high-performance, and memory-safe C++ (C++20/C++23) and C11 codebases.

---

## 1. Dialect Boundaries & Interoperability

Projects often feature distinct subsystem tiers with varying language standards:
- **Core Native Runtime**: Strict **C11** (`-std=c11`). Focuses on flat memory image virtualization, low-overhead POSIX/Linux syscall wrappers, and zero-overhead execution.
- **Client Application / UI**: Modern **C++23** (`-std=c++23`). Combines platform windowing (SDL3, GLFW), OpenGL/Vulkan contexts, and reactive UI models (e.g. RmlUi).
- **GPU Translation / High-Performance Subsystems**: Modern **C++23** (`-std=c++23`). Leverages standard library improvements, standard concepts, ranges, and Vulkan 1.3 features.

### Guidelines:
1. **Never leak C++ standard headers into C11 modules**:
   `src/` files must compile strictly with standard C11 compilers (`gcc -std=c11` or `clang -std=c11`).
2. **Explicit `extern "C"` on ABI boundaries**:
   Headers shared between C and C++ subsystems must be wrapped in `#ifdef __cplusplus extern "C" { #endif`.
3. **Header Guards**:
   - Standard C++ headers must use `#pragma once`.
   - Dual C/C++ shared boundary headers may use standard `#ifndef` guards to maintain broad compiler portability.

---

## 2. Resource Management & RAII

1. **Zero Naked Allocations**:
   - Never use raw `new` / `delete` or `malloc` / `free` in C++ components.
   - Use `std::make_unique<T>()` for exclusive ownership.
   - Use `std::make_shared<T>()` only when lifetime is non-hierarchical and truly shared across threads.
   - Prefer standard containers (`std::vector`, `std::string`, `std::array`, `boost::container::small_vector`) over dynamically allocated raw C arrays.

2. **Handle Wrapper Idiom**:
   - Wrap OS file descriptors, pipes, SDL windows, and OpenGL/Vulkan resources in RAII classes that guarantee closure/release upon scope exit or during stack unwinding.
   ```cpp
   // PASS: RAII UniqueFd for file descriptors and pipes
   class UniqueFd {
   public:
     explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
     ~UniqueFd() noexcept { Reset(); }
     UniqueFd(const UniqueFd &) = delete;
     UniqueFd &operator=(const UniqueFd &) = delete;
     UniqueFd(UniqueFd &&other) noexcept : fd_(other.Release()) {}
     UniqueFd &operator=(UniqueFd &&other) noexcept {
       if (this != &other) { Reset(other.Release()); }
       return *this;
     }
     int Get() const noexcept { return fd_; }
     bool Valid() const noexcept { return fd_ >= 0; }
     int Release() noexcept { int tmp = fd_; fd_ = -1; return tmp; }
     void Reset(int new_fd = -1) noexcept {
       if (fd_ >= 0) { ::close(fd_); }
       fd_ = new_fd;
     }
   private:
     int fd_ = -1;
   };
   ```

3. **SDL3 Resource Cleanup**:
   ```cpp
   // PASS: std::unique_ptr with custom deleter for SDL objects
   struct SdlWindowDeleter { void operator()(SDL_Window* w) const { if (w) SDL_DestroyWindow(w); } };
   using UniqueSdlWindow = std::unique_ptr<SDL_Window, SdlWindowDeleter>;
   ```

---

## 3. Concurrency & Thread Synchronization

1. **Scoped Locking**:
   - Always acquire mutexes with `std::scoped_lock` or `std::lock_guard`.
   - Never hold a lock across blocking I/O calls, child process waits, or cross-thread notification dispatches.
   ```cpp
   // PASS: scoped lock with minimal duration
   {
     std::lock_guard<std::mutex> lock(pending_mutex_);
     console_log.append(incoming_chunk);
   }
   NotifyUiThread(); // Outside the lock!
   ```

2. **Thread Lifetime Management**:
   - In C++20, prefer `std::jthread` over `std::thread` to provide automatic cooperative cancellation (`std::stop_token`) and guaranteed join on destruction.
   - Avoid `std::thread(...).detach()` unless the thread's lifecycle is permanently bound to the process lifetime and completely independent of any object state.

3. **Atomics and Memory Ordering**:
   - Use `std::atomic<T>` for cross-thread flags, exit states, and monotonic sequence counters.
   - Specify memory order explicitly where synchronization semantics matter:
     - `std::memory_order_relaxed` for independent metrics/counters.
     - `std::memory_order_release` / `std::memory_order_acquire` for synchronized handoffs.

---

## 4. Naming & Formatting Conventions

- **Types, Structs & Classes**: `PascalCase` (e.g., `LauncherModel`, `ConfigManager`).
- **Public Domain Functions & Methods**: `PascalCase` (`LaunchGame`, `ValidateGameDirectory`).
- **Internal / Helper Functions**: `snake_case` or `camelCase`.
- **Variables & Arguments**: `snake_case` (`output_res`, `user_dir`).
- **Private Member Variables**: `trailing_underscore_` (e.g., `window_`, `pending_mutex_`).
- **Constants & Macros**: `UPPER_SNAKE_CASE` (e.g., `DEFAULT_POOL_SIZE`).
- **Header Guards**: `#pragma once` for all C++ headers.

---

## 5. Static Analysis & Linting

Projects should provide automated validation commands:
```bash
# Format check
clang-format --dry-run --Werror src/*.cpp src/*.h

# Static analysis with clang-tidy
clang-tidy -p build/ path/to/file.cpp
```

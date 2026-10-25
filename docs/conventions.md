# Repository Conventions: Directory Layout, File Naming, and Dialect Standards

This document formalizes the naming conventions, directory structure, language boundaries, and architectural guidelines across the **Bloodborne PC** repository.

---

## 1. Directory Structure & Responsibilities

The codebase is organized into explicit, decoupled functional domains:

```text
bloodborne_pc/
├── src/                 # Native C11/C++23 Runtime Layer
├── core/                # C++23 Core Translation Engine (GPU, Vulkan, bbnet, upscalers)
├── launcher/            # C++23 Client Launcher (SDL3, OpenGL 3.3, RmlUi 6.x)
├── tests/               # CTest automated test suites
├── tools/               # Offline extractors, diagnostic scripts, dump inspection
├── scripts/             # Deployment, packaging, and execution helpers
├── docs/                # Architecture documentation and Architecture Decision Records (ADRs)
└── out/                 # Build artifacts (managed via CMake presets & Ninja)
```

### 1.1 Subsystem Roles

- **`src/` (Native Execution Layer)**:
  - Responsible for guest ELF/eboot loading, memory virtualization (`mmap`, page traps, direct physical memory pools), syscall shims, dynamic symbol registry, and POSIX subsystem bridges (`file`, `savedata`, `rtc`, `content`, `pad`, etc.).
  - Standard: Modern **C++23** (`-std=c++23`) for runtime services and subsystems; strict **C11** (`-std=c11`) exclusively for low-level segment register manipulation (`GS`/`FS`) and assembly trampolines (`runtime_host.c`, `runtime_thread.c`).

- **`core/` (Core Translation Engine)**:
  - Formerly referred to as `gpu/`. Responsible for GPU command processing (GNM/PM4), Vulkan rendering translation, depth-adaptive TAA, frame generation/reconstruction, upscalers (FSR 3/4, DLSS bridge), guest CPU/camera hooks, and the `bbnet` multiplayer subsystem.
  - Standard: Modern **C++23** (`-std=c++23`).

- **`launcher/` (Client GUI)**:
  - Native standalone graphical client for game configuration, audio/display settings, save profiles, and process streaming.
  - Standard: Modern **C++23** (`-std=c++23`). Zero Python/external runtime dependencies.

- **`tests/` (Verification Suites)**:
  - Automated integration, unit, and behavioral tests executed via CTest.
  - Standard: Modern **C++23** (`-std=c++23`), linked against project subsystems without mocking internal logic unnecessarily.

---

## 2. File Naming & Extension Conventions

1. **C++ Header Files (`.hpp`)**:
   - All C++ subsystem interfaces must use the `.hpp` extension.
   - Must be self-contained and guarded with `#pragma once`.
   - When declaring C-linkage guest ABI functions, wrap them in `extern "C" { ... }`.

2. **C++ Source Files (`.cpp`)**:
   - All C++ implementations must use the `.cpp` extension.
   - Internal helper routines and non-exported types must reside within dedicated namespaces (e.g., `namespace runtime::pad`, `namespace bb::vfs`) or anonymous namespaces.

3. **C Header Files (`.h`)**:
   - Reserved strictly for pure C11 interfaces and guest ABI definition headers (e.g., `runtime.h`, `host_sync.h`, `runtime_error.h`).
   - Must provide `#ifdef __cplusplus extern "C" { #endif` guards.

4. **C Source Files (`.c`)**:
   - Strictly reserved for C11 assembly trampolines and low-level thread context bridges (`runtime_host.c`, `runtime_thread.c`).
   - New subsystem implementations must not use `.c`.

5. **Naming Casing**:
   - Directories and source files: strictly `snake_case` in lower-case ASCII (e.g., `runtime_memory.cpp`, `config_manager.hpp`, `test_savedata.cpp`).
   - Test files: strictly prefixed with `test_<subsystem>.cpp`.

---

## 3. Build System Standards

1. **Official Build Engine**:
   - **CMake 3.24+** combined with **Ninja** is the primary, canonical build system.
   - All configuration, compilation, and testing workflows must be driven by standard CMake facilities (`CMakePresets.json`).

2. **Standard Targets**:
   - `cmake --preset default` / `cmake --build --preset default`: standard compilation.
   - `ctest --preset default`: runs the test suite.
   - `cmake --build --preset lint`: runs static analysis & formatting checks.
   - `cmake --build --preset format`: formats modified sources according to `.clang-format`.

3. **Helper Scripts**:
   - Task runners like `justfile` and legacy scripts like `scripts/build.sh` serve strictly as thin wrappers around CMake Presets and must not duplicate compilation flags or build logic.

# Fork Enhancements & Additions

This document details the features, architectural overhauls, and resilience improvements introduced in this fork of **Bloodborne PC**.

---

## 1. 100% Native C++20 / SDL3 / RmlUi Launcher

The previous launcher implementation relied on Python scripts (`bbport_launcher.py`), external GUI bindings (PyQt/Tkinter), and interpreted subprocess management. 

In this fork, the entire client launcher has been rewritten as a native, compiled C++20 binary:
- **Zero Python Runtime Dependencies**: The launcher runs natively with `out/bb-launcher`, eliminating Python virtual environments, pip packages, and interpreter startup latency.
- **SDL3 & Modern OpenGL Backend**: Window creation, input handling, and presentation run directly via SDL3 and OpenGL 3.3 Core.
- **RmlUi 6.x Reactive Data Binding**: UI layout is defined in clean RML markup (`launcher/assets/launcher.rml`) and RCSS stylesheets (`launcher/assets/style.rcss`), bound directly to C++ view model properties via `Rml::DataModelHandle`.

---

## 2. Adaptive & Responsive UI Architecture

- **Tiling Window Manager Support**: The launcher cleanly adapts to arbitrary window dimensions, split tiling layouts (Hyprland, Sway, i3), and high-DPI displays.
- **Structured Column Layout**: Setting rows use a table-cell layout with proportional widths (`col-info: 42%`, `col-control: 58%`), ensuring labels, subtitles, segmented buttons, and input fields never wrap or overlap.
- **Automatic Scroll Reset**: When switching navigation tabs (*The Hunt*, *Display & Graphics*, *Controls & Input*, *Network & Online*), the viewport scroll offset is automatically reset to the top (`scroll_top = 0`), preventing settings in shorter panes from being rendered off-screen.

---

## 3. Native OS Folder Choosers

- Clicking on the folder paths or titles (*Game Directory*, *Saves & Data Path*, *Modifications Path*) opens the system's native folder picker.
- Implemented natively using `SDL_ShowOpenFolderDialog` with asynchronous callback handling and a fallback to `zenity --file-selection --directory` when running in environments without portal support.

---

## 4. Interactive Collapsible Console Drawer & Live Process Streaming

- **Non-blocking Process Execution**: Game launch uses UNIX pipe redirection (`pipe`, `fork`, `execvp`) to capture `stdout` and `stderr` in real-time without freezing the UI.
- **Collapsible Terminal Drawer**: A dedicated terminal interface (`#console-drawer`) monitors engine initialization, relocation patching, memory mapping, shader compilation, and runtime messages.
- **Real-time Status Monitoring**: Live process lifecycle tracking displays active state pills (`RUNNING` / `STOPPED`) and exit status notifications.

---

## 5. Direct Log Management (Copy & Save)

- **One-Click Copy**: Copies the full initialization log to the system clipboard via `SDL_SetClipboardText`, featuring transient visual confirmation feedback (`COPIED!`).
- **Save Log to File**: Opens a native save file dialog (`SDL_ShowSaveFileDialog` / `zenity --file-selection --save`) to export logs for debugging and bug reporting.
- **Log Clear**: Instantly purges accumulated terminal logs on demand.

---

## 6. Resilient Build System & Automated Fallbacks

- Integrated automatic CMake `FetchContent` fallbacks for `Vulkan-Headers`, `VulkanMemoryAllocator` (VMA), `tsl-robin-map`, and `magic_enum`.
- Enables seamless builds across diverse Linux environments (Arch, Debian, Ubuntu, NixOS) even when specific distro development headers are absent.

---

## 7. Built-in PS4 PKG & Disc ISO Extraction

- **Integrated Launcher Extraction**: Users can install and extract legally acquired PS4 `.pkg` packages and disc `.iso` images directly within the launcher UI via the **Install / Extract Dump** action.
- **Standalone CLI Automation**: Dumps can also be extracted headlessly using `just extract <file.pkg|file.iso> [destination]` or `tools/extractor/extract_dump.sh`.
- **Reference Architecture**: Built using the proven decryption and PlayStation File System (PFS) extraction methodology from [ShadPs4Plus](https://github.com/AzaharPlus/shadPS4Plus) and [ShadPKG](https://github.com/seregonwar/ShadPKG) (AES-128-XTS PFS decryption, HMAC key derivation, and direct userspace ISO9660/UDF extraction).
- **Automatic Configuration**: Once extraction finishes, the launcher automatically detects `eboot.bin`, sets `game_dir` in `bbport.toml`, and marks the installation as verified and ready.

---

## 8. Modern TOML Configuration (`bbport.toml`)

- Fully migrated the configuration format from legacy unstructured `.ini` files to typed, modern **TOML** (`bbport.toml`).
- Provides clean typing (quoted strings, booleans `true`/`false`, numbers), structured defaults, and seamless backward-compatible loading.

---

## 9. Cross-Platform Justfile Task Runner

- Added a root `justfile` providing standard cross-platform developer workflows:
  - `just build`: Builds the runtime, GPU layer, and launcher.
  - `just test`: Runs the automated test suite.
  - `just lint`: Runs clang-tidy and formatting verification.
  - `just format`: Automatically formats codebase according to `.clang-format`.
  - `just run`: Directly executes the game runtime.
  - `just launcher`: Launches the graphical client.
  - `just extract`: Extracts PKG or ISO dump files.

---

## 10. Preliminary Windows Support (MSYS2 CLANG64)

- Introduced preliminary Windows compilation and execution support based on PR #6:
  - Native Win32 synchronization primitives (`src/host_sync.h`) replacing POSIX pthreads with SRW Locks and Condition Variables.
  - Win32 memory management and page protection (`src/win32_memory.c/.h`) using `VirtualAlloc2` and bottom-up memory mapping.
  - Win32 filesystem and runtime bridges (`src/win32_compat.c/.h`).
  - Windows execution scripts (`scripts/run.bat`, `scripts/run_windows.py`).

---

## 11. C++ & C Coding Standards and Quality Tooling

- Standardized the codebase against modern coding conventions (`cpp-coding-standards`):
  - Strict RAII for all system resources (pipes, file descriptors, synchronization handles).
  - Clean inclusion guards (`#pragma once`).
  - Automated formatting and linting via `.clang-format` and `.clang-tidy`.

---

## 12. Real-Time Extraction Progress & Destination Selection

- **Interactive Progress Bar**: When extracting PKG packages or ISO disc images in the launcher, an animated progress bar and real-time file counters provide live extraction status (`[PROGRESS] <pct>|<detail>`).
- **Destination Folder Dialog**: Selecting a dump file immediately prompts the user with a native folder selection dialog to specify where the game files should be unpacked before starting.

---

## 13. System Platform Abstraction Layer (PAL)

- Centralized low-level operating system facilities under `src/platform/` with unified C11 interfaces:
  - `src/platform/platform.h`: OS and architecture identification macros (`BB_PLATFORM_LINUX`, `BB_PLATFORM_WINDOWS`, `BB_PLATFORM_MACOS`).
  - `src/platform/sync.h`: Unified lightweight locks, condition variables, and host thread wrappers.
  - `src/platform/time.h`: High-precision monotonic and realtime timestamps, deadline sleeping, and timezone calculations.
  - `src/platform/memory.h`: Virtual address space reservation, committed anonymous allocations, and page protection.
  - `src/platform/system.h`: High-entropy randomness, host thread naming, guest TCB/TLS configuration, and CPU core counting.
  - `src/runtime_platform.c`: Native implementations for Linux, Windows, and preparation for future macOS ports.

---

## 14. Supermedo Fork Improvements Integration

- **Windows CA Trust Verification**: Integrated dynamic Windows Root CA certificate validation (`gpu/bbnet/include/windows_trust.h`) via Schannel fallback, allowing secure online connectivity when host root stores are updated dynamically.
- **Pad Route Replay Benchmark Fix**: Updated replay playback in `src/runtime_pad.c` to reload route files fresh on every replay start instead of caching once.
  - Integrated `--lint` and `--format` modes in `scripts/build.sh` and `justfile`.

- **Automated `FetchContent` Fallbacks in CMake**:
  - Automatically fetches official Khronos `Vulkan-Headers` if missing on the host OS.
  - Automatically fetches `VulkanMemoryAllocator` (VMA) without requiring manual installation of AUR packages.
  - Automatically fetches `tsl-robin-map`, `magic_enum`, and `xbyak` when not provided by system package managers.
- **Submodule Hardening**: `scripts/build.sh` updates only active GPU submodules (`fsr-vulkan`, `imgui`) without touching or conflicting with existing third-party directories.
- **Proactive Dependency Diagnostics**: `scripts/build.sh` detects missing development headers (`<vulkan/vulkan.h>`, `<boost/...>`) and outputs clear, distro-specific package installation commands.

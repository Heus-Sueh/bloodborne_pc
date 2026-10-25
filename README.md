<div align="center">
  <h1>Bloodborne PC Port</h1>
  <p><strong>Native Linux &amp; Windows Execution Layer for Bloodborne (PS4)</strong></p>

  <p align="center">
    <a href="docs/cpp-coding-standards.md">Coding Standards</a>
    •
    <a href="docs/fork-enhancements.md">Fork Enhancements</a>
    •
    <a href="AGENTS.md">Agent Guidelines</a>
    •
    <a href="bbport.toml.example">Config Template</a>
  </p>

  <p align="center">
    <img src="https://img.shields.io/badge/Language-C11%20%2F%20C%2B%2B20%20%2F%20C%2B%2B23-blue.svg" alt="Language" />
    <img src="https://img.shields.io/badge/Platform-Linux%20x86--64%20%7C%20Windows-orange.svg" alt="Platform" />
    <img src="https://img.shields.io/badge/Graphics-Vulkan%201.3-red.svg" alt="Vulkan" />
    <img src="https://img.shields.io/badge/Launcher-SDL3%20%2B%20RmlUi-green.svg" alt="Launcher" />
    <img src="https://img.shields.io/badge/Config-TOML-purple.svg" alt="TOML" />
  </p>
</div>

---

# Overview

**Bloodborne PC** is an x86-64 native execution layer for *Bloodborne* (PlayStation 4, CUSA03173 / CUSA00900, update 1.09).

Unlike traditional emulators, the game's original x86-64 machine code executes directly on host CPU cores without instruction translation or JIT compilation. PS4 system libraries and POSIX/kernel interfaces are bridged by a lightweight native C11 runtime, while PS4 GPU command buffers (GNM/PM4) are dynamically translated into modern Vulkan 1.3 commands.

> [!IMPORTANT]
> **No copyrighted assets are provided.** You must supply your own legally obtained dump of *Bloodborne* with update 1.09 (retail CUSA00900 or GOTY CUSA03173).
> This project is an independent community development and is not affiliated with Sony Interactive Entertainment, FromSoftware, or AMD.

> [!IMPORTANT]
> **Hardware Requirements:** Requires an x86-64 CPU and a GPU with full **Vulkan 1.3** support (AMD RDNA/GCN, NVIDIA Pascal or newer, Intel Arc / Iris Xe).

---

# Setup & Installation

### 1. Dump Your Game

You must dump your own legally acquired copy of *Bloodborne* from your PlayStation 4 console. Both raw folder dumps, Fake Packages (`.pkg`), and disc images (`.iso`) are supported.

The target directory structure must contain:
```text
CUSA03173/
├── eboot.bin
├── dvdroot_ps4/
│   ├── shader/
│   │   └── gxshader.shaderbnd.dcx
│   └── ...
└── sce_sys/
```

### 2. Built-in Dump Extraction (PKG & ISO)

You can extract your `.pkg` or `.iso` dump directly through the graphical launcher or via the CLI:

- **Via Launcher**: In the **The Hunt** tab, click **EXTRACT (PKG / ISO)**, select your game dump, and the launcher will decrypt, unpack, and configure the game directory automatically.
- **Via Justfile**:
  ```bash
  just extract /path/to/Bloodborne.pkg ./games/CUSA03173
  # or
  just extract /path/to/Bloodborne.iso ./games/CUSA03173
  ```

### 3. Launching

Start the graphical launcher:
```bash
just launcher
# or
./launcher/bb-launcher.sh
```

Or launch the game directly:
```bash
just run
# or
./scripts/run.sh
```

---

# Configuration

All configuration is stored in typed, modern **TOML** format (`bbport.toml`). The launcher automatically creates and maintains this file.

```toml
# Bloodborne PC Port Configuration
game_dir = "/path/to/CUSA03173"
user_dir = "user"
mods_dir = "mods"
output_res = "2560x1440"
live_resolution = "auto"
fullscreen = false
borderless = false
vsync = true
fps_limit = 60
upscaler = "fsr"
preset = 0
online_pool = "offline"
online_server = "play.seamlesshunt.com:31313"
mouse_look = true
mouse_sensitivity = 1.0
mouse_invert_y = false
```

See [`bbport.toml.example`](bbport.toml.example) for a full list of configuration keys.

---

# Building

This project utilizes a unified cross-platform workflow powered by [`just`](https://github.com/casey/just) and `build.sh`.

### Dependencies

#### Arch Linux / Omarchy:
```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf git \
    vulkan-headers vulkan-icd-loader sdl3 freetype2 \
    boost robin-map zydis glslang xxhash ffmpeg protobuf miniupnpc
```

#### Ubuntu / Debian (24.04+):
```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config git \
    libvulkan-dev libsdl3-dev libfreetype-dev \
    libboost-dev libxxhash-dev glslang-tools libzydis-dev \
    libavcodec-dev libavformat-dev libprotobuf-dev libminiupnpc-dev
```

#### Windows (MSYS2 CLANG64):
Install [MSYS2](https://www.msys2.org/) and run from the `CLANG64` environment:
```bash
pacman -S --needed mingw-w64-clang-x86_64-clang \
    mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja \
    mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-vulkan-loader \
    mingw-w64-clang-x86_64-sdl3 mingw-w64-clang-x86_64-freetype \
    mingw-w64-clang-x86_64-boost mingw-w64-clang-x86_64-robin-map \
    mingw-w64-clang-x86_64-zydis mingw-w64-clang-x86_64-xxhash \
    mingw-w64-clang-x86_64-ffmpeg
```

### Build Commands

```bash
# Build native runtime, GPU translation layer, and launcher
just build
# or: ./scripts/build.sh

# Run comprehensive test suite
just test
# or: ./scripts/build.sh --test

# Run static analysis and formatting check
just lint
# or: ./scripts/build.sh --lint

# Automatically format C/C++ files
just format
# or: ./scripts/build.sh --format
```

---

# Fork Enhancements & Additions

This fork introduces significant architectural, usability, and quality improvements:

- 🚀 **100% Native C++20 / SDL3 / RmlUi Launcher:** Replaced all legacy Python GUI scripts with a compiled, instant-launch native client (`out/bb-launcher`).
- 📦 **Integrated PKG & ISO Extraction:** Built-in userspace extraction for PS4 PKG archives (AES-128-XTS PFS decryption based on [ShadPs4Plus](https://github.com/AzaharPlus/shadPS4Plus) and [ShadPKG](https://github.com/seregonwar/ShadPKG)) and ISO disc images (`bsdtar` / `7z`).
- ⚙️ **TOML Configuration Format:** Fully migrated from legacy `.ini` to typed `.toml` (`bbport.toml`).
- 🛠️ **Cross-Platform Justfile:** Unified build, test, lint, format, run, and extraction recipes via `just`.
- 🪟 **Preliminary Windows Support:** Win32 synchronization layer (`src/host_sync.h`), virtual memory management (`src/win32_memory.c/.h`), and Windows launcher (`scripts/run.bat`).
- 📐 **Adaptive Tiling Layout:** Fully responsive UI designed for tiling window managers (Hyprland, Sway, i3) with native OS file dialogs and live log streaming.
- 🛡️ **Coding Standards & Static Analysis:** Enforces modern RAII and C++ guidelines (`cpp-coding-standards`, `.clang-format`, `.clang-tidy`).
- 🌐 **100% English Documentation:** Cleaned and standardized all documentation and comments in English.

See [`docs/fork-enhancements.md`](docs/fork-enhancements.md) for full technical documentation.

---

# Coding Standards

All contributions must follow our coding conventions:
- **C11** for low-level runtime components (`src/`).
- **C++23** for GPU translation and networking (`core/`).
- **C++20** for client launcher applications (`launcher/`).
- Strict RAII for system resources, explicit synchronization, and zero raw owning pointers.

Refer to [`docs/cpp-coding-standards.md`](docs/cpp-coding-standards.md) and [`AGENTS.md`](AGENTS.md).

---

# Credits & License

- Based on the native Linux execution layer for Bloodborne and the shadPS4 translation project.
- PKG extraction methodology derived from [ShadPs4Plus](https://github.com/AzaharPlus/shadPS4Plus) and [ShadPKG](https://github.com/seregonwar/ShadPKG).
- Licensed under the **GNU General Public License v2.0 or later** (GPL-2.0-or-later).

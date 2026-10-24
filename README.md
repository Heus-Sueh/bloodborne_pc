# Bloodborne PC Port

[![Language](https://img.shields.io/badge/Language-C11%20%2F%20C%2B%2B20%20%2F%20C%2B%2B23-blue.svg)](docs/cpp-coding-standards.md)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86--64-orange.svg)]()
[![Graphics](https://img.shields.io/badge/Graphics-Vulkan%201.3-red.svg)]()
[![UI](https://img.shields.io/badge/Launcher-SDL3%20%2B%20RmlUi-green.svg)]()

A native, high-performance Linux PC port of *Bloodborne* (PlayStation 4, CUSA03173 / CUSA00900, update 1.09). 

The game's original x86-64 executable executes directly on host hardware without CPU emulation or instruction translation layers. System calls and OS services are fulfilled by a dedicated native C11 runtime, while PS4 GPU command buffers (GNM/PM4) are translated on-the-fly into native Vulkan 1.3 commands.

---

> [!IMPORTANT]
> **No copyrighted assets are provided.** You must supply your own legally obtained dump of *Bloodborne* with update 1.09 (any retail SKU, such as US CUSA00900 or EU GOTY CUSA03173).
> This project is an independent community development and is not affiliated with Sony Interactive Entertainment, FromSoftware, or AMD.

> [!IMPORTANT]
> **Graphics Requirements:** Requires an x86-64 Linux environment with a GPU supporting **Vulkan 1.3+** (AMD RDNA/GCN, NVIDIA Pascal+, or modern Intel Arc/Iris Xe).

---

## Setup & Quick Start

1. **Dump Your Game:** Dump your legally purchased copy of *Bloodborne* (version 1.09) from your PS4 console. The directory must contain `eboot.bin` and the `dvdroot_ps4/` asset folder.
2. **Build or Download the Launcher:**
   ```bash
   ./build.sh
   ./launcher/bb-launcher.sh
   ```
3. **Configure Paths:** In the launcher interface, click **Game Directory** to select your dumped game folder using the native system folder chooser.
4. **Hunt:** Click **START THE HUNT** to launch into Yharnam!

---

## Features & Enhancements

- **Direct Native Execution:** Zero CPU emulation overhead. The game's x86-64 machine code runs at native speed directly on host CPU cores.
- **Decoupled Multi-Threaded GPU Pipeline:** GNM/PM4 command buffer decoding and Vulkan draw recording are distributed across parallel worker threads, eliminating single-core bottlenecks.
- **Unlocked Framerate:** In-engine simulation hooks synchronize physics to actual frame deltas (~90 FPS at 4K, ~150 FPS at 1440p on modern GPUs; toggle 30 FPS cap anytime with **F3**).
- **Arbitrary Aspect Ratios:** Seamless live support for 16:9, 16:10, 21:9, and 32:9 ultrawide with HUD anchoring and cinematic adaptation.
- **Full Keyboard & Mouse Camera Control:** Direct mouse look turning, context-sensitive cursor in menus, Dark Souls III style bindings, and dynamic PlayStation/Xbox controller prompts.
- **Temporal Motion Reconstruction:** Built-in motion vector generation pipeline feeding AMD FSR 3.1, FSR 4 (INT8/FP8), and NVIDIA DLSS.
- **Online Multiplayer & Seamless Co-op:** Built-in support for **Seamless Hunt** (BBSC co-op for up to 5 hunters) and **The Hunter's Dream** pools.
- **Virtual Mod Filesystem:** Case-insensitive mod overlay (`mods/`) that seamlessly overrides game assets without touching the original dump.

---

## Fork Enhancements & Additions

This fork introduces a major modern overhaul to the user interface, system integration, and build reliability:

- 🚀 **100% Native C++20 / SDL3 / RmlUi Launcher:** Completely replaced all legacy Python launcher dependencies (`bbport_launcher.py`, PyQt/Tkinter) with a compiled, instant-launch native client (`out/bb-launcher`).
- 📐 **Adaptive, Tiling-Friendly Layout:** Responsive layout engineered with proportional table-cell scaling that seamlessly handles window resizing, 4K scaling, and tiling window managers (Hyprland, Sway, i3).
- 📁 **Native OS Folder Choosers:** Integrated `SDL_ShowOpenFolderDialog` (with zenity fallback) allowing one-click directory selection by clicking directly on path labels and textboxes.
- 💻 **Interactive Collapsible Console Drawer:** Integrated live terminal drawer streaming process stdout and stderr in real-time through non-blocking UNIX pipes.
- 📋 **Integrated Log Actions:** One-click log copying with transient visual confirmation (`COPIED!`), native file save dialogs, and log clearing.
- 🛡️ **Resilient CMake Build System:** Added automatic `FetchContent` fallbacks for Khronos `Vulkan-Headers`, `VulkanMemoryAllocator` (VMA), `tsl-robin-map`, and `magic_enum`, enabling smooth builds even on minimal Linux installations.

See [docs/fork-enhancements.md](docs/fork-enhancements.md) for full technical architectural details.

---

## Building from Source

### 1. Install Dependencies

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

#### NixOS:
Run inside the provided environment:
```bash
nix-shell --run "./build.sh"
```

### 2. Compile

```bash
# Compile client launcher, GPU layer, and runtime
./build.sh

# Run automated unit test suite
./build.sh --test
```

---

## Documentation

- [AGENTS.md](AGENTS.md) — Autonomous agent instructions and architecture guidelines.
- [C++ & C Coding Standards](docs/cpp-coding-standards.md) — Coding conventions, RAII rules, and RmlUi guidelines.
- [Fork Enhancements](docs/fork-enhancements.md) — Complete overview of additions in this fork.
- [Modifications Guide](docs/MODS.md) — Virtual filesystem layout and mod installation.
- [Motion Vector Architecture](docs/motion_vectors.md) — Mathematical reconstruction of motion vectors.
- [Parallel GPU Pipeline](docs/parallel_gpu.md) — Design notes on multi-threaded PM4 decoding.
- [Temporal Upscaler Pipeline](docs/upscaler.md) — Temporal AA, FSR 3.1/4, and DLSS integration.

---

## Credits & Acknowledgements

- **FromSoftware & Sony Interactive Entertainment** for creating *Bloodborne*.
- The **shadPS4** development team for pioneer research in PS4 GPU translation.
- The **BBSC & Seamless Hunt** community for online co-op protocol developments.
- Inspired by modern native game decompilation projects like [Dusklight](https://github.com/TwilitRealm/dusklight).

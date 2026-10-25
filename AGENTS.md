# AGENTS.md — Repository Guidance for Autonomous AI Agents

This file provides architectural context, build sequences, development constraints, and guidelines for AI coding agents operating within the **Bloodborne PC** repository.

---

## 1. Project Overview & Subsystem Boundaries

The project is an x86-64 native Linux execution layer for *Bloodborne* (PlayStation 4, CUSA03173 / CUSA00900, update 1.09), replacing PS4 system libraries and translating GPU commands to Vulkan without CPU emulation.

```
bloodborne_pc/
├── src/                 # Native C11 Runtime (process loader, memory virtualization, POSIX/syscall bridges)
├── gpu/                 # C++23 GPU Translation Core (GNM/PM4 command processor, Vulkan translation, FSR/DLSS)
│   ├── bbnet/           # Online multiplayer subsystem (shadNet client, HTTPS, UPnP, BBSC seamless co-op)
│   └── third_party/     # Submodules (fsr-vulkan, imgui, sirit)
├── launcher/            # C++20 Client Launcher (SDL3, OpenGL 3.3 Core, RmlUi 6.x)
│   ├── src/             # Launcher controller, model, config persistence, process pipe streaming
│   └── assets/          # RML templates, RCSS stylesheets, TrueType fonts
├── docs/                # Architecture documentation, ADRs, coding standards (100% English)
├── out/                 # Build artifacts directory (binaries, logs, temporary overlays)
├── build.sh             # Unified compilation script
└── run.sh               # Execution entry point
```

---

## 2. Key Commands & Feedback Loops

### Building the Project
```bash
# Using Justfile (cross-platform runner)
just build
just test
just lint
just format

# Or unified shell script
./build.sh
./build.sh --test
./build.sh --lint
./build.sh --format

# Extract game dumps (PKG / ISO)
just extract /path/to/game.pkg ./games/CUSA03173
# or
./tools/extractor/extract_dump.sh /path/to/game.pkg ./games/CUSA03173
```

### Running the Launcher & Game
```bash
# Launch the native graphical interface
just launcher
# or: ./launcher/bb-launcher.sh
# or: ./out/bb-launcher

# Launch game directly (requires valid game directory in bbport.toml)
just run
# or: ./run.sh
# or: run.bat (Windows)
```

---

## 3. Core Development Rules & Constraints

### Language & Toolchain
- **Runtime (`src/`)**: Strict **C11** (`-std=c11`). No C++ in the core runtime.
- **GPU Core (`gpu/`)**: Modern **C++23** (`-std=c++23`).
- **Launcher (`launcher/`)**: Modern **C++20** (`-std=c++20`). Zero Python dependencies.
- **Documentation**: All documentation must be maintained in **English**. Do not create or commit localized `.ru.md` or non-English files.

### C++ Coding Standards
- Refer to [`docs/cpp-coding-standards.md`](docs/cpp-coding-standards.md).
- Strict RAII for all system resources (pipes, file descriptors, SDL windows, GL contexts).
- Explicit synchronization via `std::mutex`, `std::lock_guard`, and `std::atomic<T>`.

### RmlUi & UI Styling Constraints
When modifying `launcher/assets/launcher.rml` or `launcher/assets/style.rcss`:
1. **Universal Block Reset**: RmlUi has no built-in user agent stylesheet; all elements default to `display: inline`. Every stylesheet must maintain the block reset:
   ```css
   div, p, h1, h2, h3, h4, section, header, footer { display: block; box-sizing: border-box; }
   ```
2. **Single-String Font Families**: `font-family` in RCSS accepts only a single string identifier (e.g. `font-family: Noto Sans;`), never comma-separated fallback lists.
3. **Viewport Resets**: Always reset container scroll offsets (`content->SetScrollTop(0.0f)`) when switching navigation tabs.
4. **Valid XML**: Every RML tag must be explicitly closed (e.g. `<input type="text" />`, `<br/>`).

---

## 4. Dependencies & Automated Fallbacks

- **Arch Linux / Omarchy host packages**:
  ```bash
  sudo pacman -S --needed vulkan-headers boost robin-map zydis sdl3 freetype2
  ```
- **CMake Automated Fallbacks**:
  - `Vulkan-Headers`: Fetched via `FetchContent` if system headers are missing.
  - `VulkanMemoryAllocator`: Fetched via `FetchContent` if VMA is missing.
  - `tsl-robin-map`, `magic_enum`, and `xbyak`: Automatically fetched if not provided by host.

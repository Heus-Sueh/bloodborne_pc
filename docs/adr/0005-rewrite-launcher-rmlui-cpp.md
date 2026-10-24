# 0005. Rewrite Launcher in C++20 with RmlUi and Retire Launcher Python Subsystem

Date: 2026-10-24

## Status

Accepted

## Context

The Bloodborne PC launcher was previously a 2000+ line Python script (`launcher/bbport_launcher.py`) that depended on PyGObject, GTK 4, and libadwaita. This introduced significant runtime overhead, fragile system library dependencies across Linux distributions (often necessitating a heavy Nix shell wrapper), startup latency, and multi-language maintenance friction across Python and C/C++. Auxiliary launcher helpers (`bbport_vulkan.py`, `bbport_assets.py`, `bbport_i18n.py`) added further fragmentation.

## Decision

1. **Vendored RmlUi Core & Backends**: Vendored RmlUi directly into `third_party/RmlUi/` with static compilation (`out/libRmlUi.a`), utilizing native SDL3 platform handling and modern OpenGL 3 renderer bindings.
2. **Native C++20 Architecture**:
   - `launcher/src/config_manager.h/.cpp`: Thread-safe, dependency-free INI parser and game environment manager responsible for `bbport.ini`, directory validation (checking `eboot.bin`), FSR 4 asset verification, and launching `run.sh`.
   - `launcher/src/launcher_model.h/.cpp`: Reactive ViewModel following RmlUi Data Model patterns, exposing reactive properties (`output_res`, `upscaler`, `online_pool`, `mouse_look`, `fps_limit`, etc.) and event callbacks (`play`, `set_tab`, `save`) without leaking raw DOM or RmlUi handles into domain logic.
   - `launcher/src/main.cpp`: SDL3 + GL3 event loop running at low resource footprint, managing window resizing, input events, and font rendering. Explicitly initializes the glad OpenGL 3 loader via `RmlGL3::Initialize()` after `SDL_GL_MakeCurrent` to prevent uninitialized shader function pointer dereferences.
   - `launcher/assets/`: RCSS styles and RML templates establishing a clean Bloodborne-themed UI.
3. **Phase 1 Python Retirement**:
   - Removed `launcher/bbport_launcher.py`, `launcher/bbport_vulkan.py`, `launcher/bbport_assets.py`, `launcher/bbport_i18n.py`, `launcher/shell.nix`, and `launcher/__pycache__`.
   - Updated `launcher/bb-launcher.sh` to execute the native binary `out/bb-launcher` directly.
   - Replaced Python launcher tests with native C++ unit tests (`tests/test_launcher.cpp`).
4. **Static Analysis & Formatting**:
   - Verified clean under `clang-tidy` (`bugprone-*`, `clang-analyzer-*`, `performance-*`).
   - Formatted using `clang-format`.

## Consequences

### Positive
- Zero Python, GTK 4, or PyGObject dependencies for the launcher.
- Lightning-fast launcher startup and minimal memory footprint (~6 MB self-contained binary).
- Fully native C++20 code base with single-command compilation (`bash build.sh`).
- Seamless Steam Deck and desktop integration with direct `--play` headless shortcut support.
- Adherence to RmlUi design rules (R-01 to R-07), keeping declarative styling and reactive models strictly decoupled.

### Negative / Trade-offs
- Core boot scripts in `scripts/` (e.g. `prepare.py`, `link_libc.py`, `patches.py`) remain in Python for Phase 1 and will be ported in subsequent phases.

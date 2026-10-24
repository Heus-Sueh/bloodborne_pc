# Domain Glossary

Canonical vocabulary for the `bloodborne_pc` domain and its architectural seams.

## Terms

### SymbolRegistry
The central in-process module in the native C runtime (`src/runtime_registry.c`, `src/runtime.h`) responsible for resolving guest executable imports to host implementations. Modules register their exported symbols at boot into a static, fixed-capacity open-addressing hash table (2048 slots). Presents a single $O(1)$ interface to the loader (`probe.c`) for both function and data symbols, falling back to `bbgpu_resolve` for graphics imports.

### OrbisErrorAdapter
The canonical adapter (`src/runtime_error.h`, `src/runtime_error.c`) translating host POSIX `errno` codes and synchronization statuses into PS4 Orbis error codes (`0x80020000 | code`). Consolidates error mapping, timeout translation (`orbis_error_from_timed`), and error code definitions (`ORBIS_*`) across all runtime subsystems to eliminate divergent local error handling.

### RuntimeExport
The typed metadata record registering an implementation at the `SymbolRegistry` seam: specifying the symbol's name or NID, its target host address, and whether it satisfies a function or data import (`EXPORT_FUNCTION` vs `EXPORT_DATA`).

### HostPlatform
The deep module (`gpu/bbhost.h`, `gpu/shim/window.cpp`, `gpu/shim/sdl_window.cpp`) encapsulating host OS interactions: SDL windowing, event filtering, relative mouse capture for camera control, in-game IME text dialogs, and game pad input filtering. Exposes focused `bbhost_*` procedures to the runtime without exposing any graphics or Vulkan types.

### GameHooks
The deep module (`gpu/bbhooks.h`, `gpu/shim/bbport_guest_hooks.cpp`) encapsulating reverse-engineered guest game mechanics: camera rotation vectors, free-camera toggles, projection parameters, and guest code patch installation. Exposes focused `bbhook_*` procedures without binding to either windowing or GPU rendering pipelines.

### GpuRenderer
The deep module (`gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`) encapsulating Vulkan device management, presenter swapchains, shader rasterization, GPU page fault trapping, and guest graphics NID resolution. Exposes minimal lifecycle and fault dispatch entry points (`bbgpu_init`, `bbgpu_resolve`, `bbgpu_handle_fault`).

### OnlineSubsystem
The standalone dynamic module (`gpu/bbnet/`, producing `libbbnet.so`) implementing the guest PS4 network (`sceNet`, `sceHttp`, `sceSsl`) and NP matchmaking/signaling stacks. Supports both the *Seamless Hunt* (shadNet + BBSC) and *The Hunter's Dream* (THD) backends behind a unified C host seam (`BbNetHost` in `bbnet.h`), eliminating duplicate module trees and runtime environment flags.

### MemoryCoherence
The deep subsystem (`src/runtime_memory.c`, `src/runtime.h`) coordinating guest CPU memory mappings with GPU VRAM mirrors and rasterizer caches. Encapsulates table generation sequence tracking, thread-local region caching (`runtime_memory_check_mapped`), write-trap management, and unified GPU invalidation notifications (`RuntimeMemoryCoherenceHooks`), hiding internal pointer arithmetic and atomic sequence flags from callers.

### NativeLauncher
The standalone C++20 executable (`out/bb-launcher`, `launcher/src/main.cpp`) providing the user interface for configuring and starting the port. Uses SDL3 for windowing and event handling, OpenGL 3.3 Core for rendering, and RmlUi for reactive HTML/CSS-like document rendering. Replaces the legacy Python GTK4 application with a zero-Python, low-overhead native runtime.

### LauncherViewModel
The reactive data binding layer (`launcher/src/launcher_model.h/.cpp`) connecting the RmlUi document DOM to the underlying `ConfigManager`. Binds user settings (`output_res`, `upscaler`, `online_pool`, `mouse_look`, etc.) to RmlUi Data Model properties and event callbacks without leaking RmlUi types into core system or configuration logic.

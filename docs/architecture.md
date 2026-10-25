# Bloodborne PC Architecture Specification

## 1. Architectural Philosophy

The **Bloodborne PC** architecture is designed to execute PlayStation 4 *Bloodborne* (CUSA03173 / CUSA00900, update 1.09) natively on modern x86-64 operating systems without CPU emulation. The system translates OrbisOS kernel services and translates GNM/PM4 GPU command buffers to Vulkan.

The architectural layout incorporates design principles inspired by **Aurora** ([`encounter/aurora`](https://github.com/encounter/aurora)) and **N64ModernRuntime** ([`N64Recomp/N64ModernRuntime`](https://github.com/N64Recomp/N64ModernRuntime)):
- **Decoupled Subsystem Layers**: Each major system (Kernel, Memory, VFS, Input, Audio) maintains an explicit contract (`BbSubsystemContract`) with managed initialization and teardown lifecycles.
- **Platform Abstraction Layer (PAL)**: All operating-system-specific facilities reside behind unified, minimal interfaces in `src/platform/`.
- **Callback Hook Architecture**: Core subsystems expose clean hook registration points for frontend presentation, audio sink delivery, and input event streaming.
- **Deterministic Virtual File System (VFS)**: Hierarchical asset resolution resolves files across base game, title updates, DLC add-ons, user mod overlays, and isolated saves.

---

## 2. System Topology

```
┌────────────────────────────────────────────────────────┐
│                   Launcher Frontend                     │
│       (C++20, SDL3, RmlUi 6.x, bbport.toml, VFS UI)     │
└───────────────────────────┬────────────────────────────┘
                            │
┌───────────────────────────▼────────────────────────────┐
│          Bloodborne PS4 Executable (eboot.bin)         │
│       Native x86-64 Execution (No CPU Emulation)       │
└───────────────────────────┬────────────────────────────┘
                            │
┌───────────────────────────▼────────────────────────────┐
│      OrbisOS Runtime Subsystems (src/) - C11           │
│  (Inspired by ultramodern & Aurora OS Layer)           │
│  ├── Kernel Core (Threads, Sync, Events, Sems, Timers) │
│  ├── Memory & Coherence (VMA, Trapping, Direct Pools)  │
│  ├── Virtual File System (AppContent, PFS, Overlays)   │
│  ├── Input & Controls (PAD, Motion, Touch, Remap)      │
│  └── Audio Subsystem (ATRAC9, Stream, Voice Mixer)     │
└──────────────┬──────────────────────────┬──────────────┘
               │                          │
┌──────────────▼────────────┐ ┌───────────▼──────────────┐
│  Platform Abstraction     │ │  GPU & Presentation Core  │
│  Layer (PAL - src/platform│ │  (gpu/) - Modern C++23    │
│  Linux / Win32 / macOS    │ │  (Inspired by Aurora GX) │
│  ├── Threads & Sync       │ │  ├── GNM/PM4 Processor    │
│  ├── High-Res Clocks      │ │  ├── Vulkan Pipeline/Desc│
│  ├── Virtual Address Space│ │  ├── Upscalers (FSR/DLSS) │
│  └── System & Entropy     │ │  └── Online Co-op (bbnet) │
└───────────────────────────┘ └──────────────────────────┘
```

---

## 3. Subsystem Breakdown

### 3.1 Kernel Subsystem (`BB_SUBSYSTEM_KERNEL`)
- **Responsibilities**: OrbisOS userland kernel simulation: threads, fiber scheduling, mutexes, condition variables, semaphores, event queues (`kqueue`), and timer interrupts.
- **Contract Interface**: `src/runtime_kernel.c`, `src/runtime_thread.c`.
- **Platform Mapping**: Maps guest POSIX threading constructs to PAL synchronization primitives (`src/platform/sync.h`).

### 3.2 Memory & Coherence Subsystem (`BB_SUBSYSTEM_MEMORY`)
- **Responsibilities**:
  - Virtual address space reservation (`0x1000000000` base) and bottom-up memory layout.
  - Direct and flexible memory management (`sceKernelAllocateDirectMemory`, `sceKernelMapDirectMemory`).
  - Speculative write watch and trapping engine with fault recovery handlers.
  - CPU-GPU rasterizer coherence seam (`RuntimeMemoryCoherenceHooks`).
- **Contract Interface**: `src/runtime_memory.c`, `src/platform/memory.h`.

### 3.3 Virtual File System Subsystem (`BB_SUBSYSTEM_VFS`)
- **Responsibilities**: Unified guest filesystem routing with layered resolution precedence:
  1. `VFS_LAYER_BASE`: Decrypted PlayStation File System (PFS) or extracted PKG/ISO disc tree (`/app0/dvdroot/`).
  2. `VFS_LAYER_PATCH`: Update 1.09 patch directory overlay.
  3. `VFS_LAYER_DLC`: Add-on content mounts (`/addcont/...`).
  4. `VFS_LAYER_MOD`: User-defined mod directory overlays (`mods/`).
  5. `VFS_LAYER_SAVE`: Isolated read-write user save directory (`savedata/`).
- **Contract Interface**: `src/runtime_vfs.h`, `src/runtime_content.c`, `src/runtime_file.c`.

### 3.4 Input & HID Subsystem (`BB_SUBSYSTEM_INPUT`)
- **Responsibilities**: DualShock 4 emulation, axis deadzone compensation, gyro/motion translation, touch pad coordinate mapping, chord shortcuts (debug camera, frame pause), and input route replay for reproducible performance benchmarks.
- **Contract Interface**: `src/runtime_pad.c`.

### 3.5 Audio Subsystem (`BB_SUBSYSTEM_AUDIO`)
- **Responsibilities**: Multi-channel ATRAC9 hardware decoding simulation via `third_party/LibAtrac9`, voice mixing, NGX sound playback, and output sample streaming to host sinks.
- **Contract Interface**: `src/runtime_audio.c`.

---

## 4. Platform Abstraction Layer (PAL)

The PAL resides in `src/platform/` and exposes a pure C11 API:

| Header | Description | Backends |
| :--- | :--- | :--- |
| `platform.h` | Platform & architecture detection macros (`BB_PLATFORM_LINUX`, `BB_PLATFORM_WINDOWS`, `BB_PLATFORM_MACOS`). | Preprocessor definitions |
| `sync.h` | Fast host mutexes, recursive locks, read-write locks, condvars, and host thread wrappers. | Linux (`pthreads`), Windows (`SRWLOCK`, `CONDITION_VARIABLE`), macOS (`pthreads`). |
| `time.h` | Monotonic clock, realtime clock, high-precision deadline sleep, and UTC timezone offset. | Linux (`clock_gettime`, `clock_nanosleep`), Windows (`QPC`, high-res timer), macOS (`clock_gettime`). |
| `memory.h` | Virtual address space reservation, committed private memory, and page protection. | Linux (`mmap`), Windows (`VirtualAlloc2`, `win_mem_*`), macOS (`mmap` / Mach VM). |
| `system.h` | Cryptographic entropy, thread naming for debuggers, guest TCB/TLS assignment, page sizes, and CPU counts. | Linux (`getrandom`, `pthread_setname_np`), Windows (`BCrypt`, `SetThreadDescription`), macOS (`arc4random_buf`, `pthread_setname_np`). |

---

## 5. Dialect & Boundary Invariants

1. **C11 Strictness**: The runtime layer (`src/`) must strictly compile under `-std=c11` without C++ dependencies.
2. **C++23 Modernity**: The GPU translation layer (`gpu/`) utilizes modern C++23 features (concepts, ranges, std::span, modules where applicable).
3. **C++20 Launcher**: The graphical client (`launcher/`) utilizes modern C++20 with SDL3 and RmlUi 6.x.
4. **Documentation Integrity**: All technical documentation, code comments, and ADRs are maintained in 100% English.

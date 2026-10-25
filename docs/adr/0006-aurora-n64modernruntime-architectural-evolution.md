# 0006. Architectural Evolution Inspired by Aurora and N64ModernRuntime

Date: 2026-10-24

## Status

Accepted

## Context

As the execution layer for Bloodborne expands across platforms (Linux x86_64, Windows x86_64, and future macOS Darwin) and introduces complex subsystems (GPU Vulkan translation, online multiplayer, native launcher, mod loading, and audio decoding), the internal architecture requires clear subsystem boundaries and standardized abstraction layers.

Prior to this architectural evolution:
1. Low-level platform primitives (mutexes, memory mapping, sleeps, randomness, threads) were partially mixed across `src/runtime_*.c`, with platform `#ifdef _WIN32` branches scattered throughout core runtime logic.
2. The runtime subsystems operated with implicit lifecycle couplings, lacking an explicit contract for initialization, status reporting, and teardown.
3. Game assets, patch files, DLC add-ons, and mod overlays lacked a formalized layered virtual file system (VFS) resolution hierarchy.

## Inspiration & Design Provenance

We adopt proven architectural patterns from two landmark modern console compatibility and static recompilation runtimes:

1. **Aurora** ([`encounter/aurora`](https://github.com/encounter/aurora)):
   - **Subsystem Decoupling**: Strict isolation of OS, graphics (GX over modern APIs), input (PAD via SDL3), storage (DVD/CARD), and audio streaming.
   - **Platform Abstraction Layer (PAL)**: Clean platform isolation hosting identical guest execution logic across Linux, Windows, macOS, and mobile.
   - **Frontend Separation**: Completely independent frontend clients communicating with the execution core.

2. **N64ModernRuntime** ([`N64Recomp/N64ModernRuntime`](https://github.com/N64Recomp/N64ModernRuntime)):
   - **`ultramodern` Pattern**: Modular reimplementation of console operating system facilities (threads, message queues, timers, fiber scheduling) independent of binary origin.
   - **Hook & Callback Architecture**: Callback-driven seams for controller polling, audio buffer streaming, and video presentation.
   - **`librecomp` VFS & Overlays**: Structured layered virtual filesystem routing ROM/disc assets, title updates, mod overlays, and atomic save redirection.

## Decision

1. **Establish a Dedicated Platform Abstraction Layer (PAL)**:
   - Centralize all host platform interfaces in `src/platform/` (`platform.h`, `sync.h`, `time.h`, `memory.h`, `system.h`).
   - Implement native platform backends (`src/runtime_platform.c`) for Linux, Windows, and macOS Darwin preparation.
   - Ensure runtime subsystems (`src/runtime_*.c`) consume `bb_platform_*` interfaces instead of issuing raw syscalls or platform-specific headers.

2. **Introduce Standardized Subsystem Lifecycle Contracts**:
   - Provide `src/runtime_subsystems.h` and `src/runtime_subsystems.c` defining `BbSubsystemContract` for:
     - `BB_SUBSYSTEM_KERNEL`: Threads, fibers, event queues, semaphores, timers.
     - `BB_SUBSYSTEM_MEMORY`: Address space virtualization, direct memory pools, coherence hooks.
     - `BB_SUBSYSTEM_VFS`: Decrypted PFS, title updates, DLC add-ons, mod overlays, atomic saves.
     - `BB_SUBSYSTEM_INPUT`: DualShock 4 mapping, deadzone calculation, touch/mouse emulation, replay routing.
     - `BB_SUBSYSTEM_AUDIO`: ATRAC9 decoding workers, voice mixing, audio output sinks.
   - Enforce deterministic ordered initialization via `bb_subsystems_init()` and teardown via `bb_subsystems_shutdown()`.

3. **Formalize the Layered Virtual File System (VFS)**:
   - Introduce `src/runtime_vfs.h` with deterministic resolution precedence:
     `Base PFS` < `Title Update 1.09` < `DLC Add-on` < `Mod Overlay` < `User Save`.

4. **Preserve Language & Toolchain Boundaries**:
   - Runtime core (`src/`): Strict **C11** (`-std=c11`).
   - GPU translation (`gpu/`): Modern **C++23** (`-std=c++23`).
   - Launcher client (`launcher/`): Modern **C++23** (`-std=c++23`).

## Consequences

### Positive
- High cohesion and low coupling across runtime subsystems.
- Seamless multi-platform portability without cluttering core game translation logic with `#ifdef` directives.
- Clean hook seams for frontend integration, headless test harnesses, and profiling tools.
- Preserves 100% backward compatibility with `eboot.bin` guest symbols and existing automated test suites.

### Negative / Trade-offs
- Requires maintaining explicit subsystem contract interfaces in `src/runtime_subsystems.h`.
- Developers must use `src/platform/` abstraction functions instead of directly invoking platform-specific APIs.

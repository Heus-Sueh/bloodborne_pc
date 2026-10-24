# 0002. Decompose the bbgpu God-Interface into HostPlatform, GameHooks, and GpuRenderer

Date: 2026-10-24

## Status

Accepted

## Context

The `gpu/bbgpu.h` interface previously functioned as an umbrella "god-interface" declaring over 25 functions spanning three unrelated domains:
1. Host window and input management (SDL3 relative mouse, event dispatch, IME text prompts, pad filters).
2. Game-specific reverse engineering mechanics (`BbGuestHooks` camera look, free camera, projection state, code patching).
3. Vulkan graphics rendering, page fault signal trapping, and NID resolution.

Because all these responsibilities were bundled together behind `gpu/bbgpu.h`:
- Modules like `src/runtime_pad.c` and `src/runtime_services.c` had to import the GPU header despite performing zero rendering work.
- `src/runtime_rewatch.c` had to use an undeclared extern prototype for `bbgpu_camera_state` to avoid pulling in the full GPU interface.
- Standalone test runners (`pad-test`, `test_runtime`, `sema-test`) required large sets of weak dummy stubs for GPU rendering functions just to link windowing and pad logic.

## Decision

Decompose `gpu/bbgpu.h` into three distinct deep modules with focused, dedicated seams:
1. `HostPlatform` (`gpu/bbhost.h`): Declares window management, relative mouse capture, IME dialogs, pad filters, and overlay banner queries (`bbhost_*`).
2. `GameHooks` (`gpu/bbhooks.h`): Declares game-specific camera manipulation vectors, free camera status, camera state queries, and image patching (`bbhook_*`).
3. `GpuRenderer` (`gpu/bbgpu.h`): Retains core Vulkan initialization, kernel event queues, page fault dispatch, and NID resolution (`bbgpu_*`).

To ensure complete backward compatibility with existing C++ code in `gpu/games/` and `gpu/bbnet/`:
- `gpu/bbgpu.h` includes `bbhost.h` and `bbhooks.h`.
- Backward-compatibility aliases and forwarding functions are provided so existing `bbgpu_*` calls continue to work seamlessly across all translation units.

## Consequences

### Positive
- `src/runtime_pad.c` and `src/runtime_services.c` no longer depend on graphics or Vulkan headers.
- Information hiding is significantly enhanced: windowing and camera hook details are isolated from the graphics engine.
- Standalone test runners need only lightweight host or hook stubs, simplifying test harness maintenance.
- Stronger cohesion and clearer seams across runtime, host platform, and game mechanics.

### Negative / Trade-offs
- Introduced two new header files (`gpu/bbhost.h` and `gpu/bbhooks.h`).
- Forwarding functions maintained for ABI compatibility during the transition.

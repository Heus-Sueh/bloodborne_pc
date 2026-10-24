# 0004. Deepen the Memory Coherence Seam Between CPU Runtime and GPU Rasterizer

Date: 2026-10-24

## Status

Accepted

## Context

The memory subsystem previously exposed a porous, leaky boundary between the C native runtime (`src/runtime_memory.c`) and the GPU rasterizer (`gpu/shim/bbgpu.cpp`):
1. Raw internal generation sequence pointer (`runtime_memory_generation()`) was exported to C++, forcing `bbgpu.cpp` to duplicate an ad-hoc thread-local region cache (`RegionCache`), manually performing bitwise `generation & 1` checks and atomic loads across the module boundary.
2. The GPU rasterizer registered its invalidation, mapping, unmapping, note-write, and CPU-write callbacks across 3 separate global setter functions with 5 loose function pointer arguments (`runtime_memory_set_gpu_hooks`, `runtime_memory_set_note_write_hook`, `runtime_memory_set_cpu_write_hook`).

## Decision

Deepen and encapsulate the memory coherence seam:
1. Provide a single, unified `RuntimeMemoryCoherenceHooks` structure and registration function (`runtime_memory_set_coherence_hooks`) uniting all five memory lifecycle callbacks.
2. Implement `runtime_memory_check_mapped(address, size)` entirely within `src/runtime_memory.c`, encapsulating table generation sequence validation, lockless fast-path thread-local region caching, and multi-VMA boundary clamping.
3. Replace the duplicated `RegionCache` and generation pointer probing in `gpu/shim/bbgpu.cpp` with a clean call to `runtime_memory_check_mapped`.

## Consequences

### Positive
- Information hiding: raw `table_generation` atomic pointer and bitwise sequence flags are encapsulated inside `runtime_memory.c`.
- Eliminates duplicated thread-local caching structures in `gpu/shim/bbgpu.cpp`.
- A single coherent registration seam for rasterizer hooks.
- Simplifies testing and guarantees consistent memory region validation between CPU and GPU sides.

### Negative / Trade-offs
- None. Backwards-compatibility wrapper setters are retained for any legacy callers.

# 1. Centralized Symbol Registry and Canonical Orbis Error Adapter

Date: 2026-10-24

## Status

Accepted

## Context

Guest imports in `bloodborne_pc` were resolved through an 18-stage linear cascade of module resolve functions (`runtime_*_resolve`), where each module performed linear scans over 989 NIDs. Additionally:
- Seven different modules duplicated private Orbis error conversion tables and `ERR(n)` macros (`0x80020000 | errno`).
- `runtime_time.c` was a 25-line shallow module with a single function and dedicated resolve hook.
- Data exports were handled via hardcoded `if` checks in `runtime.c`.

## Decision

1. **Central Symbol Registry**: Introduce a unified `SymbolRegistry` module in the C runtime using a fixed-capacity (2048-slot) open-addressing hash table populated at boot. Modules register their exports once; resolution is $O(1)$ with delegation to `bbgpu_resolve` for graphics symbols.
2. **Unified Typed Exports**: Standardize export registration on `RuntimeExport` descriptors supporting both function and data symbols.
3. **Absorb Shallow Modules**: Delete `runtime_time.c` and absorb its implementation (`guest_gettimeofday`) into `runtime_kernel.c`.
4. **Canonical Error Adapter**: Extract `runtime_error.h` / `runtime_error.c` to centralize all Orbis error mapping, constants, and timeout conversions across all runtime modules.

## Consequences

- Resolves symbols in $O(1)$ time without repetitive 989-NID table scans.
- Deletes shallow `runtime_time.c` and eliminates redundant resolve boilerplate across 18 files.
- Eliminates 7 duplicated error mapping implementations.
- Preserves clean isolation from the C++ GPU compilation unit.

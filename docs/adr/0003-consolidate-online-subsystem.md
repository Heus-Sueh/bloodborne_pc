# 0003. Consolidate Online Subsystem into Single Modern Module

Date: 2026-10-24

## Status

Accepted

## Context

The repository previously maintained two separate online module trees in `gpu/`:
1. `gpu/bbnet/`: An older, incomplete module tree based on shadPS4 v0.19 (supporting only the shadp2p backend), generating `libbbnet.so`.
2. `gpu/bbnet20/`: A second-generation module tree based on shadPS4 v0.20 and the *The Hunter's Dream* (THD) backend, generating `libbbnet20.so`.

This dual-tree setup caused considerable friction:
- Over 600 lines of identical mini-kernel glue (`bbnet_glue.cpp`) were duplicated between `gpu/bbnet/` and `gpu/bbnet20/`.
- `src/runtime_net.c` hardcoded a path to `gpu/libbbnet.so`, forcing `run.sh` and `launcher/bbport_launcher.py` to inject environment variables (`BB_NET_MODULE=.../libbbnet20.so` and `BB_NET_GEN=20`) to select the modern module.
- Build systems (`build.sh`, `gpu/CMakeLists.txt`) had to configure and build both targets independently.

## Decision

Consolidate the online subsystem around the modern v0.20 + THD implementation:
1. Delete the obsolete `gpu/bbnet/` directory and standardize on the modern codebase as the sole `gpu/bbnet/` module.
2. Build the unified module as `libbbnet.so`, creating a `libbbnet20.so` symlink for backwards compatibility with existing distribution packages and scripts.
3. Update `src/runtime_net.c` to load `libbbnet.so` natively with seamless fallback to `libbbnet20.so`.
4. Remove `BB_NET_GEN` logic and build-time duplication from `run.sh`, `build.sh`, and `gpu/CMakeLists.txt`.

## Consequences

### Positive
- Eliminated 600+ lines of duplicated glue code and redundant CMake configuration.
- Single unified seam (`BbNetHost` in `gpu/bbnet/bbnet.h`) serving both *Seamless Hunt* and *The Hunter's Dream* multiplayer pools.
- Simplified runtime loading and removed fragile environment variable overrides.
- Maintained 100% binary backwards compatibility via post-build symlinks and loader fallback.

### Negative / Trade-offs
- None. The obsolete 0.19 module was already superseded in functionality and reliability.

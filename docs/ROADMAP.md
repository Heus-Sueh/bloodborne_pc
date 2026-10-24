# Project Architecture Roadmap

This document outlines the milestones and roadmap for the native PC execution layer of Bloodborne.

## Core Milestones

1. **Native CPU Execution & Syscall Emulation** (Complete)
   - Flat memory image generation directly from retail `eboot.bin`.
   - Native C11 runtime replacement for PS4 kernel primitives (pthreads, mutexes, semaphores, virtual memory).
   - Replaced POSIX/libc and libSceFios2 with high-performance native host equivalents.

2. **Decoupled GPU Pipeline & Vulkan Translation** (Complete)
   - Translated PS4 GNM/PM4 command streams into Vulkan 1.3 command buffers.
   - Decoupled command decoding from draw recording across independent worker threads.
   - Built-in temporal motion vector synthesis for AMD FSR 3.1, FSR 4.x, and DLSS.

3. **Modern PC Client & Launcher** (Complete)
   - Rewritten launcher architecture in C++20, SDL3, and RmlUi.
   - Fully responsive, DPI-aware interface with native folder selection and real-time process monitoring.
   - Virtual filesystem overlay for mod loading and seamless configuration persistence.

4. **Future Investigations**
   - Headless automated regression testing harness.
   - Extended controller vibration and adaptive trigger mapping.
   - Shader compiler caching optimizations for instant area transitions.

# C++ & C Coding Standards

This document establishes the architecture conventions, memory safety guidelines, and code quality standards for the **Bloodborne PC Native Port** codebase across the C11 runtime layer, C++23 GPU translation subsystem, and C++20 SDL3/RmlUi client launcher.

---

## 1. Language Standards & Dialects

- **Core Runtime (`src/`)**: Written in strict **C11** (`-std=c11`). Focuses on zero-overhead OS virtualization, low-level POSIX/Linux syscall bridges, high-frequency synchronization, and flat memory image loading.
- **Client Launcher (`launcher/`)**: Written in **C++20** (`-std=c++20`). Integrates SDL3 windowing, OpenGL 3.3 Core rendering context, and RmlUi reactive UI data models.
- **Core Translation Engine (`core/`)**: Written in **C++23** (`-std=c++23`). Uses modern standard library features, standard algorithms, and Vulkan 1.3 abstractions.

---

## 2. Resource Management & RAII

1. **No Manual Memory Management**:
   - Strictly avoid naked `malloc`/`free` or manual `new`/`delete` in C++ components.
   - Use `std::unique_ptr` for exclusive ownership and `std::shared_ptr` / `std::weak_ptr` only when ownership is genuinely shared.
   - Use standard containers (`std::vector`, `std::string`, `std::array`, `boost::container::small_vector`) instead of raw dynamically allocated C arrays.
2. **Handle Wrapper Idioms**:
   - Wrap OS and library primitives (file descriptors, pipes, SDL windows, GL contexts, thread handles) inside deterministic RAII classes whose destructors release resources predictably even under exceptional unwind paths.

---

## 3. Concurrency & Thread Synchronization

1. **Mutex Scope Minimization**:
   - Protect shared mutable state with `std::mutex` accessed exclusively via `std::lock_guard` or `std::scoped_lock`.
   - Never hold a lock across blocking I/O calls or cross-thread notification dispatches.
2. **Lock-Free Primitives**:
   - Use `std::atomic<T>` for cross-thread flags, exit states, and monotonic counters.
   - Specify memory orders explicitly (`std::memory_order_relaxed`, `std::memory_order_acquire`, `std::memory_order_release`) when optimizing hot execution paths.

---

## 4. UI Architecture & RmlUi Guidelines

When modifying or extending the native launcher UI (`launcher/`):

1. **Strict Markup Conformance**:
   - All RML tags must be strictly XML-compliant: every tag must have an explicit closing tag (e.g., `<input type="text" />`, `<br/>`).
2. **Universal Block Reset**:
   - RmlUi does not include a browser-style user agent stylesheet; all elements default to `display: inline`. Structural containers must be reset explicitly (`display: block; box-sizing: border-box;`).
3. **Single-String Font Families**:
   - `font-family` declarations in RCSS must match a single loaded font name string (e.g. `font-family: Noto Sans;`), never comma-separated lists.
4. **Data Binding & Reactive State**:
   - Modify view model fields inside thread-safe scopes and notify RmlUi using `model_handle.DirtyVariable("var_name")`.
   - Use `data-if` for conditional layout mounting and `data-class-*` for dynamic UI state classes.
5. **Adaptive Layouts**:
   - Use proportional widths, CSS variables, and flexible containers (`display: table`, `display: flex`) to guarantee seamless scaling between windowed, tiled (tiling window managers like Hyprland/i3), and fullscreen viewports.

---

## 5. Naming & Formatting Conventions

- **Types, Structs & Classes**: `PascalCase` (e.g., `LauncherModel`, `ConfigManager`, `BufferCache`).
- **Functions & Methods**: `PascalCase` for public domain interfaces (`ChooseGameDirectory`, `LaunchGame`), `snake_case` or `camelCase` for internal utility routines.
- **Variables & Members**: `snake_case` for local variables (`game_dir`, `is_valid`); private member variables end with an underscore (e.g., `model_handle_`, `pending_mutex_`).
- **Constants & Enums**: `UPPER_SNAKE_CASE` or scoped `enum class` values (`ModalFlag::None`).
- **Header Guards**: `#pragma once` on all C++ headers.

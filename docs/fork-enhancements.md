# Fork Enhancements & Additions

This document details the features, architectural overhauls, and resilience improvements introduced in this fork of **Bloodborne PC**.

---

## 1. 100% Native C++20 / SDL3 / RmlUi Launcher

The previous launcher implementation relied on Python scripts (`bbport_launcher.py`), external GUI bindings (PyQt/Tkinter), and interpreted subprocess management. 

In this fork, the entire client launcher has been rewritten as a native, compiled C++20 binary:
- **Zero Python Runtime Dependencies**: The launcher runs natively with `out/bb-launcher`, eliminating Python virtual environments, pip packages, and interpreter startup latency.
- **SDL3 & Modern OpenGL Backend**: Window creation, input handling, and presentation run directly via SDL3 and OpenGL 3.3 Core.
- **RmlUi 6.x Reactive Data Binding**: UI layout is defined in clean RML markup (`launcher/assets/launcher.rml`) and RCSS stylesheets (`launcher/assets/style.rcss`), bound directly to C++ view model properties via `Rml::DataModelHandle`.

---

## 2. Adaptive & Responsive UI Architecture

- **Tiling Window Manager Support**: The launcher cleanly adapts to arbitrary window dimensions, split tiling layouts (Hyprland, Sway, i3), and high-DPI displays.
- **Structured Column Layout**: Setting rows use a table-cell layout with proportional widths (`col-info: 42%`, `col-control: 58%`), ensuring labels, subtitles, segmented buttons, and input fields never wrap or overlap.
- **Automatic Scroll Reset**: When switching navigation tabs (*The Hunt*, *Display & Graphics*, *Controls & Input*, *Network & Online*), the viewport scroll offset is automatically reset to the top (`scroll_top = 0`), preventing settings in shorter panes from being rendered off-screen.

---

## 3. Native OS Folder Choosers

- Clicking on the folder paths or titles (*Game Directory*, *Saves & Data Path*, *Modifications Path*) opens the system's native folder picker.
- Implemented natively using `SDL_ShowOpenFolderDialog` with asynchronous callback handling and a fallback to `zenity --file-selection --directory` when running in environments without portal support.

---

## 4. Interactive Collapsible Console Drawer & Live Process Streaming

- **Non-blocking Process Execution**: Game launch uses UNIX pipe redirection (`pipe`, `fork`, `execvp`) to capture `stdout` and `stderr` in real-time without freezing the UI.
- **Collapsible Terminal Drawer**: A dedicated terminal interface (`#console-drawer`) monitors engine initialization, relocation patching, memory mapping, shader compilation, and runtime messages.
- **Real-time Status Monitoring**: Live process lifecycle tracking displays active state pills (`RUNNING` / `STOPPED`) and exit status notifications.

---

## 5. Direct Log Management (Copy & Save)

- **One-Click Copy**: Copies the full initialization log to the system clipboard via `SDL_SetClipboardText`, featuring transient visual confirmation feedback (`COPIED!`).
- **Save Log to File**: Opens a native save file dialog (`SDL_ShowSaveFileDialog` / `zenity --file-selection --save`) to export logs for debugging and bug reporting.
- **Log Clear**: Instantly purges accumulated terminal logs on demand.

---

## 6. Resilient Build System & Automated Fallbacks

- **Automated `FetchContent` Fallbacks in CMake**:
  - Automatically fetches official Khronos `Vulkan-Headers` if missing on the host OS.
  - Automatically fetches `VulkanMemoryAllocator` (VMA) without requiring manual installation of AUR packages.
  - Automatically fetches `tsl-robin-map`, `magic_enum`, and `xbyak` when not provided by system package managers.
- **Submodule Hardening**: `build.sh` updates only active GPU submodules (`fsr-vulkan`, `imgui`) without touching or conflicting with existing third-party directories.
- **Proactive Dependency Diagnostics**: `build.sh` detects missing development headers (`<vulkan/vulkan.h>`, `<boost/...>`) and outputs clear, distro-specific package installation commands.

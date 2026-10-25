#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
if [[ ${1:-} == --lint || ${1:-} == --format ]]; then
    if [[ ${1:-} == --format ]]; then
        echo "Formatting C/C++ source files with clang-format..."
        clang-format -i launcher/src/*.cpp launcher/src/*.h tests/test_launcher.cpp
        echo "Formatting complete."
        exit 0
    fi
    echo "Checking C/C++ formatting with clang-format..."
    clang-format --dry-run --Werror launcher/src/*.cpp launcher/src/*.h tests/test_launcher.cpp
    echo "Static analysis & formatting check: PASSED"
    exit 0
fi
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by flake.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 freetype2 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]]; then
        if command -v nix >/dev/null && [[ -f flake.nix ]]; then
            exec env BB_IN_NIX_SHELL=1 nix develop --command bash scripts/build.sh "$@"
        elif command -v nix-shell >/dev/null && [[ -f shell.nix ]]; then
            exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash scripts/build.sh $*"
        fi
    fi
    echo 'Need pkg-config with vulkan, sdl3 and freetype2, cmake and ninja (see flake.nix).' >&2; exit 1
fi
CXX=${CXX:-g++}
if ! echo '#include <vulkan/vulkan.h>' | $CC -E - >/dev/null 2>&1; then
    echo "Notice: System Vulkan headers (<vulkan/vulkan.h>) not installed."
    echo "        Recommended for Arch/Omarchy: 'sudo pacman -S --needed vulkan-headers'"
fi
if ! echo '#include <boost/container/small_vector.hpp>' | $CXX -x c++ -E - >/dev/null 2>&1; then
    echo "Notice: Boost development headers (<boost/...>) not installed."
    echo "        Run on Arch/Omarchy: 'sudo pacman -S --needed boost robin-map zydis'"
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# Windows: MSYS2 CLANG64 shell. The GPU library is static there, the runtime adds its
# Win32 files (src/win32_*.c), runs without pthreads, and the executable keeps a console
# and bottom-up addresses (guest-visible host memory must stay below 1 TiB).
windows=
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
cstd=(-std=c11)
threads=(-pthread)
if [[ -n $windows ]]; then
    threads=()
    libraries=("${libraries[@]/-mwindows/-mconsole}")
    cstd=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00)
fi
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive gpu/third_party/fsr-vulkan gpu/third_party/imgui
fi
# Issue #124: a checkout updated from an earlier version keeps that version's patch applied, and
# the new one applies neither way ("patch does not apply"): the submodule's tree holds nothing but
# these patches, so it is reset and they are applied again.
fsr_patches_apply() {
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$@" "$PWD/$patch" || return 1
        fi
    done
}
if ! fsr_patches_apply --check 2>/dev/null; then
    echo 'FSR-Vulkan: an earlier version of the port changed it; resetting it and patching again'
    git -C gpu/third_party/fsr-vulkan checkout -f HEAD
fi
fsr_patches_apply
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" 2>&1 | tee out/gpu-cmake.log
grep '^-- Online module' out/gpu-cmake.log || true
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu $(grep -q "^build bbnet:" out/gpu/build.ninja && echo bbnet) 2>&1 | tee out/gpu-build.log; then
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# Optional DLSS (NVIDIA RTX): DLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> builds the bridge
# (gpu/dlss_bridge, the only code using NVIDIA's SDK) and puts it next to bb-probe with NVIDIA's
# libnvidia-ngx-dlss.so. Without them the DLSS upscaler is listed as unavailable.
if [[ -n ${DLSS_SDK_ROOT:-} ]]; then
    cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DDLSS_SDK_ROOT="$DLSS_SDK_ROOT" >/dev/null
    ninja -C out/dlss-bridge >/dev/null
    rm -f out/libnvidia-ngx-dlss.so.*
    cp out/dlss-bridge/libbbport_dlss.so "$DLSS_SDK_ROOT"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* out/
    cp "$DLSS_SDK_ROOT/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
    echo "DLSS bridge: out/libbbport_dlss.so"
fi
CXX=${CXX:-g++}
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
runtime_cpp=(src/runtime*.cpp)
runtime_cpp_objs=()
for src in "${runtime_cpp[@]}"; do
    obj="out/$(basename "${src%.cpp}").o"
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror "${includes[@]}" -I. -Isrc -c "$src" -o "$obj"
    runtime_cpp_objs+=("$obj")
done
link=(-no-pie)
if [[ -n $windows ]]; then
    mapfile -t gpu < out/gpu/bbgpu_link.txt
    runtime+=(src/win32_*.c)
    link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin)
fi
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" "${runtime_cpp_objs[@]}" src/vulkan_smoke.c out/libatrac9.a -lm -lstdc++ "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# GPU check for run.sh (live_resolution=auto) and the launcher's gamepad list (--gamepads).
"$CC" "${cstd[@]}" -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
# Native C++20 RmlUi launcher
if [[ ! -f out/libRmlUi.a ]]; then
    bash tools/build_rmlui.sh
fi
read -r -a ft_includes <<< "$(pkg-config --cflags freetype2 2>/dev/null || true)"
read -r -a ft_libs <<< "$(pkg-config --libs freetype2 2>/dev/null || true)"
"$CXX" -std=c++20 -O2 -Wall -Wextra -pthread \
    -DRMLUI_SDL_VERSION_MAJOR=3 \
    -DRMLUI_FONT_ENGINE_FREETYPE \
    -DRMLUI_NUM_MSAA_SAMPLES=0 \
    -Ithird_party/RmlUi/Include \
    -Ithird_party/RmlUi/Backends \
    -Isrc \
    -Ilauncher/src \
    "${includes[@]}" "${ft_includes[@]}" \
    src/config_manager.cpp \
    launcher/src/launcher_model.cpp \
    launcher/src/main.cpp \
    out/libRmlUi.a \
    "${libraries[@]}" "${ft_libs[@]}" \
    -lGL -lm \
    -o out/bb-launcher
echo "Built $PWD/out/bb-launcher"
# Native C++23 game runner
"$CXX" -std=c++23 -O2 -Wall -Wextra -pthread \
    -I. -Isrc \
    src/config_manager.cpp \
    src/loader.cpp \
    src/runner.cpp \
    -o out/bb-runner
echo "Built $PWD/out/bb-runner"
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -lm -o out/pad-test
    out/pad-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_platform.cpp src/runtime_platform.cpp -o out/platform-test
    out/platform-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_subsystems.cpp src/runtime_subsystems.cpp src/runtime_savedata.cpp src/runtime_content.cpp src/runtime_rtc.cpp -o out/subsystems-test
    out/subsystems-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_vfs.cpp src/runtime_vfs.cpp -o out/vfs-test
    out/vfs-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_loader.cpp src/loader.cpp -o out/loader-test
    out/loader-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_savedata.cpp src/runtime_savedata.cpp -o out/savedata-test
    out/savedata-test
    "$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_rtc.cpp src/runtime_rtc.cpp -o out/rtc-test
    out/rtc-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" "${runtime_cpp_objs[@]}" out/libatrac9.a -lm -lstdc++ "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc -I. tests/test_file_mods.c out/runtime_vfs.o -lstdc++ -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" "${runtime_cpp_objs[@]}" out/libatrac9.a -lm -lstdc++ "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_online.c "${runtime[@]}" "${runtime_cpp_objs[@]}" out/libatrac9.a -lm -lstdc++ "${gpu[@]}" "${libraries[@]}" -o out/online-test
    out/online-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c out/runtime_content.o -lstdc++ -o out/content-test
    out/content-test
    "$CXX" -std=c++20 -O2 -Wall -Wextra -pthread -Isrc -Ilauncher/src tests/test_launcher.cpp src/config_manager.cpp -o out/launcher-test
    out/launcher-test
fi

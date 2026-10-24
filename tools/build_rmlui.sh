#!/usr/bin/env bash
set -euo pipefail

mkdir -p out/rmlui

COMPILE_FLAGS="-std=c++20 -O2 -w -fPIC -Ithird_party/RmlUi/Include -Ithird_party/RmlUi/Source/Core -Ithird_party/RmlUi/Backends -DRMLUI_SDL_VERSION_MAJOR=3 -DRMLUI_FONT_ENGINE_FREETYPE -DRMLUI_NUM_MSAA_SAMPLES=0 $(pkg-config --cflags sdl3 freetype2)"
export COMPILE_FLAGS

compile_one() {
    src="$1"
    name=$(basename "$src" .cpp)
    dir_hash=$(echo "$src" | md5sum | cut -c1-6)
    obj="out/rmlui/${name}_${dir_hash}.o"
    if [[ ! -f "$obj" || "$src" -nt "$obj" ]]; then
        g++ $COMPILE_FLAGS -c "$src" -o "$obj"
    fi
}
export -f compile_one

# Find all sources
find third_party/RmlUi/Source/Core -name "*.cpp" > out/rmlui/sources.txt
echo "third_party/RmlUi/Backends/RmlUi_Renderer_GL3.cpp" >> out/rmlui/sources.txt
echo "third_party/RmlUi/Backends/RmlUi_Platform_SDL.cpp" >> out/rmlui/sources.txt

cat out/rmlui/sources.txt | xargs -n 1 -P "$(nproc)" bash -c 'compile_one "$@"' _

ar rcs out/libRmlUi.a out/rmlui/*.o
echo "Successfully built out/libRmlUi.a ($(du -h out/libRmlUi.a | cut -f1))"

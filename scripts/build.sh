#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

# Auto-discover cmake and ninja if not in default PATH
if ! command -v cmake >/dev/null 2>&1; then
    for c in /home/heus/.cache/uv/archive-v0/*/cmake/data/bin/cmake; do
        if [[ -x $c ]]; then export PATH="$(dirname "$c"):$PATH"; break; fi
    done
fi
if ! command -v ninja >/dev/null 2>&1; then
    for n in /home/heus/.cache/uv/archive-v0/*/ninja-*.data/scripts/ninja; do
        if [[ -x $n ]]; then export PATH="$(dirname "$n"):$PATH"; break; fi
    done
fi

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

mkdir -p out
cmake -S . -B out -G Ninja
ninja -C out

if [[ ${1:-} == --test ]]; then
    ctest --test-dir out --output-on-failure
fi

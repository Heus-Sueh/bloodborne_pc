#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

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

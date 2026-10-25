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

case "${1:-}" in
    --lint)
        cmake --build --preset lint
        echo "Static analysis & formatting check: PASSED"
        ;;
    --format)
        cmake --build --preset format
        echo "Formatting complete."
        ;;
    --test)
        cmake --build --preset default
        ctest --preset default
        ;;
    *)
        cmake --preset default
        cmake --build --preset default
        ;;
esac

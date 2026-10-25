#!/usr/bin/env bash
# Starts the Bloodborne native C++20 launcher (RmlUi + SDL3).
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
cd -- "$root"
if [[ ! -x "$root/out/bb-launcher" ]]; then
    bash "$root/scripts/build.sh"
fi
exec "$root/out/bb-launcher" "$@"

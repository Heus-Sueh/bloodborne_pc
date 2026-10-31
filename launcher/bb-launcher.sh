#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Starts the Bloodborne launcher.
# 1. Native GPU-accelerated C++ RmlUi launcher if built (zero python/gtk dependencies).
# 2. Falls back to GTK4 (directly or via nix-shell).
here=$(cd -- "$(dirname -- "$0")" && pwd)

# Priority: native fast client
if [[ -x "$here/../out/bb-launcher" ]]; then
    exec "$here/../out/bb-launcher" "$@"
elif [[ -x "$here/build/bb-launcher" ]]; then
    exec "$here/build/bb-launcher" "$@"
elif [[ -x "$here/../out/launcher/bb-launcher" ]]; then
    exec "$here/../out/launcher/bb-launcher" "$@"
fi

cmd=(python3 "$here/bbport_launcher.py" "$@")
if python3 -c 'import gi; gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")' 2>/dev/null; then
    exec "${cmd[@]}"
fi
printf -v quoted '%q ' "${cmd[@]}"
exec nix-shell "$here/shell.nix" --run "$quoted"

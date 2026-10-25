#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# extract_dump.sh - Automated PKG and ISO extractor for Bloodborne PC
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

SOURCE_FILE="${1:-}"
DEST_DIR="${2:-}"

if [[ -z "$SOURCE_FILE" ]]; then
    echo "Usage: $0 <game_dump.pkg|game_dump.iso> [destination_directory]"
    exit 1
fi

if [[ ! -f "$SOURCE_FILE" ]]; then
    echo "Error: Source dump file does not exist: $SOURCE_FILE" >&2
    exit 1
fi

SOURCE_FILE="$(realpath "$SOURCE_FILE")"

# Default destination directory
if [[ -z "$DEST_DIR" ]]; then
    DEST_DIR="$ROOT_DIR/games/CUSA03173"
fi
mkdir -p "$DEST_DIR"
DEST_DIR="$(realpath "$DEST_DIR")"

echo "================================================================================"
echo "[EXTRACT] Initializing dump extraction"
echo "  Source:      $SOURCE_FILE"
echo "  Destination: $DEST_DIR"
echo "================================================================================"

# Identify file type via magic bytes or extension
MAGIC=$(head -c 4 "$SOURCE_FILE" | cat -v 2>/dev/null || true)
IS_PKG=0
IS_ISO=0

if [[ "$SOURCE_FILE" == *.pkg ]] || [[ "$SOURCE_FILE" == *.PKG ]] || [[ "$MAGIC" == *"CNT"* ]]; then
    IS_PKG=1
elif [[ "$SOURCE_FILE" == *.iso ]] || [[ "$SOURCE_FILE" == *.ISO ]]; then
    IS_ISO=1
fi

if [[ $IS_ISO -eq 1 ]]; then
    echo "[EXTRACT] Processing Disc Image (ISO)..."
    echo "[PROGRESS] 5|Inspecting ISO file..."
    if command -v bsdtar >/dev/null 2>&1; then
        echo "[EXTRACT] Extracting with bsdtar..."
        TOTAL_FILES=$(bsdtar -tf "$SOURCE_FILE" 2>/dev/null | wc -l || echo 0)
        bsdtar -v -xf "$SOURCE_FILE" -C "$DEST_DIR" 2>&1 | awk -v total="$TOTAL_FILES" '
        /^x / {
            count++
            if (total > 0) {
                pct = int(count * 100 / total)
                if (pct > 99) pct = 99
                if (count % 10 == 0 || pct == 99) {
                    print "[PROGRESS] " pct "|Extracting: " count "/" total " files (" pct "%)"
                    fflush()
                }
            }
        }
        { print $0; fflush() }
        '
    elif command -v 7z >/dev/null 2>&1; then
        echo "[EXTRACT] Extracting with 7z..."
        7z x -y -bsp1 "$SOURCE_FILE" -o"$DEST_DIR" 2>&1 | tr '\r' '\n' | awk '
        /^[ ]*[0-9]+%/ {
            match($0, /[0-9]+%/)
            pct = substr($0, RSTART, RLENGTH - 1)
            print "[PROGRESS] " pct "|Extracting ISO: " pct "%"
            fflush()
        }
        { print $0; fflush() }
        '
    elif command -v tar >/dev/null 2>&1; then
        echo "[EXTRACT] Extracting with tar..."
        tar -xf "$SOURCE_FILE" -C "$DEST_DIR"
    else
        echo "Error: Neither bsdtar nor 7z is installed on host system." >&2
        exit 1
    fi
    chmod -R u+w "$DEST_DIR" 2>/dev/null || true
elif [[ $IS_PKG -eq 1 ]]; then
    echo "[EXTRACT] Processing PlayStation 4 Package (PKG)..."
    PKG_EXTRACTOR=""
    if [[ -x "$ROOT_DIR/out/pkg_extractor" ]]; then
        PKG_EXTRACTOR="$ROOT_DIR/out/pkg_extractor"
    elif [[ -x "$SCRIPT_DIR/pkg_extractor" ]]; then
        PKG_EXTRACTOR="$SCRIPT_DIR/pkg_extractor"
    elif command -v pkg_extractor >/dev/null 2>&1; then
        PKG_EXTRACTOR="$(command -v pkg_extractor)"
    fi

    # Fetch standalone pkg_extractor binary if not present
    if [[ -z "$PKG_EXTRACTOR" || ! -x "$PKG_EXTRACTOR" ]]; then
        echo "[EXTRACT] Standalone pkg_extractor not found. Downloading ShadPs4Plus binary..."
        mkdir -p "$SCRIPT_DIR"
        TMP_ZIP=$(mktemp --suffix=.zip)
        curl -sSL "https://github.com/AzaharPlus/shadPS4Plus/releases/download/PKG_EXTRACTOR_1_1/ShadPs4Plus-PkgExtractor-1.1-linux.zip" -o "$TMP_ZIP"
        TMP_DIR=$(mktemp -d)
        unzip -q -o "$TMP_ZIP" -d "$TMP_DIR"
        chmod +x "$TMP_DIR"/ShadPs4Plus-PkgExtractor-1.1-linux/pkg_extractor.AppImage
        "$TMP_DIR"/ShadPs4Plus-PkgExtractor-1.1-linux/pkg_extractor.AppImage --appimage-extract >/dev/null 2>&1 || true
        if [[ -f squashfs-root/usr/bin/pkg_extractor ]]; then
            cp squashfs-root/usr/bin/pkg_extractor "$SCRIPT_DIR/pkg_extractor"
            chmod +x "$SCRIPT_DIR/pkg_extractor"
            rm -rf squashfs-root
        else
            cp "$TMP_DIR"/ShadPs4Plus-PkgExtractor-1.1-linux/pkg_extractor.AppImage "$SCRIPT_DIR/pkg_extractor"
            chmod +x "$SCRIPT_DIR/pkg_extractor"
        fi
        rm -rf "$TMP_ZIP" "$TMP_DIR"
        PKG_EXTRACTOR="$SCRIPT_DIR/pkg_extractor"
        echo "[EXTRACT] Successfully installed pkg_extractor."
    fi

    echo "[EXTRACT] Executing: $PKG_EXTRACTOR \"$SOURCE_FILE\" \"$DEST_DIR\""
    echo "[PROGRESS] 10|Analyzing PKG and decrypting PFS partition..."
    printf '\n' | "$PKG_EXTRACTOR" "$SOURCE_FILE" "$DEST_DIR" 2>&1 | tr '\r' '\n' | awk '
    /Extracting file [0-9]+ of [0-9]+/ {
        for (i=1; i<=NF; i++) {
            if ($i == "file") cur = $(i+1)
            if ($i == "of") total = $(i+1)
        }
        if (total > 0) {
            pct = int(cur * 100 / total)
            if (pct > 99) pct = 99
            print "[PROGRESS] " pct "|Extracting: " cur "/" total " files (" pct "%)"
            fflush()
        }
    }
    { print $0; fflush() }
    '
else
    echo "Error: Unknown dump file format for $SOURCE_FILE (expected .pkg or .iso)" >&2
    exit 1
fi

# Locate extracted game root if nested under CUSA03173 or merge nested update
FINAL_GAME_DIR="$DEST_DIR"
for title in CUSA03173 CUSA00900 CUSA00207 CUSA00208 CUSA00299 CUSA01363 CUSA03179 CUSA03014 CUSA03023; do
    if [[ -d "$DEST_DIR/$title" && -f "$DEST_DIR/eboot.bin" && "$DEST_DIR" != *"/$title" ]]; then
        echo "[EXTRACT] Merging nested update files ($title) into game root..."
        cp -rf "$DEST_DIR/$title/"* "$DEST_DIR/"
        rm -rf "$DEST_DIR/$title"
    fi
done

if [[ -f "$DEST_DIR/CUSA03173/eboot.bin" ]]; then
    FINAL_GAME_DIR="$DEST_DIR/CUSA03173"
elif [[ ! -f "$DEST_DIR/eboot.bin" ]]; then
    FOUND=$(find "$DEST_DIR" -maxdepth 3 -name "eboot.bin" -print -quit 2>/dev/null || true)
    if [[ -n "$FOUND" ]]; then
        FINAL_GAME_DIR="$(dirname "$FOUND")"
    fi
fi

echo "[PROGRESS] 100|Extraction complete! Verifying game files..."
echo "================================================================================"
if [[ -f "$FINAL_GAME_DIR/eboot.bin" ]]; then
    echo "[EXTRACT] SUCCESS: Found eboot.bin in $FINAL_GAME_DIR"
    echo "[EXTRACT] TARGET_GAME_DIR=$FINAL_GAME_DIR"
else
    echo "[EXTRACT] Warning: Extraction finished but eboot.bin was not found directly in $FINAL_GAME_DIR"
fi
echo "================================================================================"

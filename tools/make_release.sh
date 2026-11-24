#!/usr/bin/env bash
# Build the web-flashable release into ./release/
#
# Produces ONE file that esptool.spacehuhn.com (ESPWebTool) can write at
# address 0x0, plus the three separate ESP-IDF outputs for anyone who would
# rather flash them at their own offsets.
#
# Why a single file at 0x0 rather than the three bins at their own offsets:
# ESPWebTool's own instructions say "a single merged firmware image goes to
# address 0x0", and its address field is typed by hand. One file at one
# well-known offset removes every chance of a typo putting the app over the
# partition table. The merge pads from 0x0, so the 8 KB before the P4
# bootloader at 0x2000 is written as 0xFF -- harmless, and it compresses to
# nothing on the wire.
#
# NOTE: the merged image spans the nvs and phy_init partitions that sit
# between the partition table and the app, so flashing it ERASES saved
# settings (WiFi credentials included). That is inherent to merging a
# contiguous image, not a bug. The three separate bins avoid it.
#
# Usage:  ./tools/make_release.sh          # uses ./build
#         ./tools/make_release.sh <build-dir>
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$PROJECT_DIR/build}"
RELEASE_DIR="$PROJECT_DIR/release"

# esptool CLI, not the IDF python env -- this script deliberately needs no
# sourced export.sh, matching gui-flasher/flasher.py.
ESPTOOL="$(command -v esptool || true)"
[ -n "$ESPTOOL" ] || ESPTOOL="$HOME/.local/bin/esptool"
if [ ! -x "$ESPTOOL" ] && ! command -v "$ESPTOOL" >/dev/null 2>&1; then
    echo "error: esptool not found on PATH or at ~/.local/bin/esptool" >&2
    exit 1
fi

FLASH_ARGS="$BUILD_DIR/flash_args"
if [ ! -f "$FLASH_ARGS" ]; then
    echo "error: $FLASH_ARGS not found -- run 'idf.py build' first" >&2
    exit 1
fi

# Read the offsets and files out of the build's own flash_args, so this script
# never carries a second copy of the partition layout. Line 1 is the flash
# settings; every line after it is "<offset> <relative path>".
#
# Deliberately no `mapfile`: macOS ships bash 3.2, which does not have it, and
# this script is meant to run on a stock Mac without Homebrew bash.
FLASH_SETTINGS=()
ENTRIES=()  # offset/file pairs, as esptool wants them on its command line
PAIRS=()    # "offset<TAB>relative path", for the manifest
_lineno=0
while IFS= read -r line || [ -n "$line" ]; do
    _lineno=$((_lineno + 1))
    [ -n "$(printf '%s' "$line" | tr -d '[:space:]')" ] || continue
    if [ "$_lineno" -eq 1 ]; then
        # shellcheck disable=SC2086
        set -- $line
        FLASH_SETTINGS=("$@")
        continue
    fi
    # shellcheck disable=SC2086
    set -- $line
    offset="$1"; rel="$2"
    src="$BUILD_DIR/$rel"
    if [ ! -f "$src" ]; then
        echo "error: $FLASH_ARGS lists $rel but $src is missing" >&2
        exit 1
    fi
    ENTRIES+=("$offset" "$src")
    PAIRS+=("$offset"$'\t'"$rel")
done < "$FLASH_ARGS"

if [ "${#ENTRIES[@]}" -eq 0 ]; then
    echo "error: $FLASH_ARGS lists no files" >&2
    exit 1
fi

# esptool 5 spells these with dashes; flash_args still emits the old
# underscore form that esptool accepts for compatibility. Normalise so the
# command line is unambiguous about which esptool it was written for.
SETTINGS_NORM=()
for arg in "${FLASH_SETTINGS[@]}"; do
    SETTINGS_NORM+=("${arg//_/-}")
done

CHIP="$( { sed -n 's/.*"chip"[^"]*"\([^"]*\)".*/\1/p' \
           "$BUILD_DIR/flasher_args.json" 2>/dev/null || true; } | head -1 )"
CHIP="${CHIP:-esp32p4}"

mkdir -p "$RELEASE_DIR"
MERGED="$RELEASE_DIR/photoframe-merged-0x0.bin"

echo "==> merging ${#PAIRS[@]} images for $CHIP into $(basename "$MERGED")"
"$ESPTOOL" --chip "$CHIP" merge-bin \
    -o "$MERGED" \
    "${SETTINGS_NORM[@]}" \
    --target-offset 0x0 \
    "${ENTRIES[@]}"

# The three separate bins, for the offset-per-file route.
SEPARATE_DIR="$RELEASE_DIR/separate-bins"
mkdir -p "$SEPARATE_DIR"
for pair in "${PAIRS[@]}"; do
    rel="${pair#*$'\t'}"
    cp "$BUILD_DIR/$rel" "$SEPARATE_DIR/$(basename "$rel")"
done

cp "$PROJECT_DIR/tools/release_README.md" "$RELEASE_DIR/README.md"

# --- manifest ---------------------------------------------------------------
# `describe --always` still fails on a repo with no commits yet, so fall back
# to the branch name and whether the tree is dirty.
GIT_DESC="$(git -C "$PROJECT_DIR" describe --always --dirty 2>/dev/null || true)"
if [ -z "$GIT_DESC" ]; then
    _branch="$(git -C "$PROJECT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
    _branch="${_branch:-no-git}"
    if [ -n "$(git -C "$PROJECT_DIR" status --porcelain 2>/dev/null || true)" ]; then
        GIT_DESC="$_branch (uncommitted changes)"
    else
        GIT_DESC="$_branch"
    fi
fi
# Guarded with `|| true`: under `set -o pipefail` a missing sdkconfig.h makes
# sed fail, which fails the whole pipeline and would abort the script here.
IDF_VER="$( { sed -n 's/^#define IDF_VER "\(.*\)"/\1/p' \
              "$BUILD_DIR/config/sdkconfig.h" 2>/dev/null || true; } | head -1 )"
[ -n "$IDF_VER" ] || IDF_VER="$(git -C "${IDF_PATH:-$HOME/esp/esp-idf}" describe --always 2>/dev/null || echo unknown)"

{
    echo "ESP32-P4 10.1\" Photo Frame -- flashable release"
    echo "built:     $(date -u '+%Y-%m-%d %H:%M:%SZ')"
    echo "source:    $GIT_DESC"
    echo "esp-idf:   $IDF_VER"
    echo "esptool:   $( { "$ESPTOOL" version 2>/dev/null || true; } | tail -1 )"
    echo "chip:      $CHIP"
    echo "settings:  ${SETTINGS_NORM[*]}"
    echo
    echo "ESPWebTool (https://esptool.spacehuhn.com/) -- one file:"
    echo "  0x0       $(basename "$MERGED")   ($(wc -c < "$MERGED" | tr -d ' ') bytes)"
    echo
    echo "separate-bins/ -- ${#PAIRS[@]} files at their own offsets:"
    # Sorted by offset. flash_args emits them in its own order (the app often
    # ahead of the partition table), which reads like a mistake in a manifest
    # someone is about to type addresses out of.
    for pair in "${PAIRS[@]}"; do
        offset="${pair%%$'\t'*}"
        rel="${pair#*$'\t'}"
        printf '%d\t  %-9s %s\n' "$offset" "$offset" "$(basename "$rel")"
    done | sort -n | cut -f2-
    echo
    echo "sha256:"
    ( cd "$RELEASE_DIR" && shasum -a 256 "$(basename "$MERGED")" separate-bins/*.bin )
} > "$RELEASE_DIR/MANIFEST.txt"

echo
cat "$RELEASE_DIR/MANIFEST.txt"
echo
echo "==> release written to $RELEASE_DIR"

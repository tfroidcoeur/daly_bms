#!/usr/bin/env bash
#
# Render the font specimen into docs/img/00-font-comparison.png.
#
#   ./tools/make-font-comparison.sh
#
# The specimen (port/sim/font_specimen.c) puts the misc-fixed ladder the UI
# actually uses next to the Montserrat-at-1bpp set it replaced, all through the
# real 1-bit pipeline. Kept separate from make-screenshots.sh because it is a
# diagnostic view, not one of the pages.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=docs/img/00-font-comparison.png
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p docs/img
SIM_FONT_SPECIMEN=1 SIM_SHOT_DIR="$TMP" SIM_QUIT_MS=1500 \
    timeout 30 ./build/sim >/dev/null 2>&1 || true

if [ ! -f "$TMP/00-final.ppm" ]; then
    echo "the simulator produced no frame - is ./build/sim built?" >&2
    exit 1
fi

# Nearest-neighbour: the whole point here is to see individual pixels.
convert "$TMP/00-final.ppm" -filter point -resize 200% "$OUT"
printf '  %-32s %s\n' "$(basename "$OUT")" "$(identify -format '%wx%h' "$OUT")"

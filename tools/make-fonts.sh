#!/usr/bin/env bash
#
# Generate the UI fonts.
#
#   ./tools/make-fonts.sh
#
# The UI runs on X11 misc-fixed, the bitmap family behind u8g2's
# u8g2_font_6x13_tf - which is what Waveshare's own U8g2 example for this board
# draws with. On a 1 bpp reflective panel a font that was *designed* at one bit
# beats any outline font rasterised down to one: every stem is exactly one or
# two whole pixels, so nothing is left to a threshold decision.
#
# misc-fixed ships as PCF, which lv_font_conv cannot read (it is built on
# opentype.js and rejects the format outright), so those go through our own
# tools/pcf_to_lvgl.py. The LV_SYMBOL_* glyphs have no bitmap equivalent and are
# merged in from FontAwesome, thresholded.
set -euo pipefail

cd "$(dirname "$0")/.."

SRC=build/_deps/lvgl-src/scripts/built_in_font
AWESOME="$SRC/FontAwesome5-Solid+Brands+Regular.woff"
X11=/usr/share/fonts/X11/misc

if [ ! -f "$AWESOME" ]; then
    echo "LVGL sources not fetched yet - run cmake -S . -B build first" >&2
    exit 1
fi
if [ ! -f "$X11/6x13.pcf.gz" ]; then
    echo "X11 misc-fixed missing - sudo apt install xfonts-base" >&2
    exit 1
fi

# Printable ASCII, plus the degree sign, plus exactly the LV_SYMBOL_* we use:
#   F021 refresh, F071 warning, F077 up, F078 down
ASCII='0x20-0x7E,0xB0'
SYMBOLS='0xF021,0xF071,0xF077,0xF078'

mkdir -p ui/fonts
PCF=$(mktemp -d)
trap 'rm -rf "$PCF"' EXIT

echo "misc-fixed (the UI's fonts):"
# strike  size  output name   symbol px  scale  [bold = synthesise a bold cut]
#
# The symbol size is set below the cell height on purpose: FontAwesome's icons
# fill their em box, so rendering one at the cell height gives a triangle taller
# than the capitals it sits next to.
#
# The last rung is 6x13 bold at 2x. misc-fixed's Latin strikes stop at 10x20,
# and the one 12x24 in the package is a different (near-serif, JIS) design that
# does not belong with the rest. Doubling keeps the family.
while read -r STRIKE SIZE NAME SYMPX SCALE BOLD; do
    gunzip -c "$X11/$STRIKE.pcf.gz" > "$PCF/$STRIKE.pcf"
    [ "$BOLD" = "bold" ] && EMB=--embolden || EMB=
    .venv/bin/python tools/pcf_to_lvgl.py \
        --pcf "$PCF/$STRIKE.pcf" --size "$SIZE" --name "$NAME" \
        --range "$ASCII" \
        --symbol-font "$AWESOME" --symbols "$SYMBOLS" --symbol-size "$SYMPX" \
        --scale "$SCALE" $EMB -o "ui/fonts/$NAME.c"
done <<'TABLE'
6x13   13 fixed_6x13     10 1
6x13B  13 fixed_6x13b    10 1
7x14   14 fixed_7x14     11 1
7x14B  14 fixed_7x14b    11 1
9x15   15 fixed_9x15     12 1
9x15B  15 fixed_9x15b    12 1
10x20  20 fixed_10x20    16 1
10x20  20 fixed_10x20b   16 1 bold
6x13B  13 fixed_6x13b_2x 10 2
TABLE

echo
echo "declared in ui/fonts/fonts.h, which maps UI_FONT_* onto them"

#!/usr/bin/env bash
#
# Render every screen, in every state worth looking at, into docs/img/.
#
#   ./tools/make-screenshots.sh
#
# Needs a built ./build/sim and a vcan interface (./tools/setup-vcan.sh).
# Images are the panel's true 400x300 scaled 2x.
#
# The scaling is smoothed by default. That is not flattery: at 400x300 across a
# 4.2 inch diagonal the panel runs about 119 DPI, so its pixels are roughly
# 0.21 mm and invisible at arm's length - a nearest-neighbour blow-up looks far
# blockier than the glass ever will. Set SHOT_PIXELS=1 for a pixel-exact
# nearest-neighbour version instead, which is what you want when hunting a
# one-pixel layout bug.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=docs/img
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$OUT"
find "$OUT" -maxdepth 1 -name '[0-9][0-9]-*.png' -delete
rm -f "$OUT/ui-pages.png"

# capture <tmp-subdir> <scenario> <sim-script> <quit-ms>
capture() {
    ./tools/capture-shots.sh "$TMP/$1" "$2" "$3" "$4" >/dev/null
}

FILTER=()
if [ -n "${SHOT_PIXELS:-}" ]; then
    FILTER=(-filter point)
fi

# save <tmp-subdir> <ppm-basename> <output-name>
save() {
    convert "$TMP/$1/$2.ppm" "${FILTER[@]}" -resize 200% "$OUT/$3.png"
    printf '  %-32s %s\n' "$3.png" "$(identify -format '%wx%h' "$OUT/$3.png")"
}

echo "walking the pages (weak-cell)..."
capture walk weak-cell "6000:n,1200:n,1200:n,1200:N" 14000
save walk 00-page   01-overview
save walk 01-page   06-pack-1
save walk 02-page   07-pack-2
save walk 03-page   08-pack-3
save walk 04-final  10-cells-pack-3

echo "cell detail with the drift fully developed..."
capture cells weak-cell "40000:n,1200:n,1200:N" 45000
save cells 03-final 11-cells-pack-2-drifted

echo "overview states..."
capture chg charging "" 7000
save chg 00-final    02-overview-charging

capture winch winch "" 16000
save winch 00-final  03-overview-winch-pull

capture faults faults "" 9000
save faults 00-final 04-overview-alarms

echo "a pack dropping off the bus..."
# The simulator stops answering for pack 3 at 15 s and the poller needs its 5 s
# timeout on top of that, so the first shot has to be later than 20 s or the
# overview is captured still showing three packs online.
capture off offline "22000:n,1000:n,1000:n" 26000
save off 00-page   05-overview-pack-offline
save off 03-final  09-pack-3-offline

# The montage the README embeds.
convert "$OUT/01-overview.png" "$OUT/04-overview-alarms.png" "$OUT/11-cells-pack-2-drifted.png" \
        +append -bordercolor '#999' -border 2 "$OUT/ui-pages.png"
printf '  %-32s %s\n' "ui-pages.png" "$(identify -format '%wx%h' "$OUT/ui-pages.png")"

echo
echo "wrote $(ls "$OUT"/*.png | wc -l) images to $OUT/"

#!/usr/bin/env bash
#
# Drive the GUI simulator against a fresh simulator and save a PNG per page.
#
#   ./tools/capture-shots.sh <out-dir> [scenario] [SIM_SCRIPT] [quit-ms]
#
# Example:
#   ./tools/capture-shots.sh shots weak-cell "6000:n,1200:n,1200:N" 12000
#
# Why the care over killing strays: two simulator processes on one vcan
# interface both answer the same requests, and their readings interleave into
# values that look exactly like a decoder bug but are not. A capture that leaves
# its simulator running poisons every later run.
set -euo pipefail

OUT=${1:?usage: capture-shots.sh <out-dir> [scenario] [script] [quit-ms]}
SCEN=${2:-normal}
SCRIPT=${3:-}
QUIT=${4:-8000}

cd "$(dirname "$0")/.."

# Find real simulator processes.
#
# `pkill -f daly_sim` is NOT safe here: any shell whose command line merely
# mentions the script - including the one invoking this - matches, and gets
# killed. So match on argv[0] instead, which is the interpreter for a real
# simulator and /bin/bash or similar for anything that just names it.
sim_pids() {
    local pid argv0
    for pid in $(pgrep -f "daly_sim\.py" 2>/dev/null || true); do
        [ "$pid" = "$$" ] && continue
        argv0=$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | head -1)
        case "${argv0##*/}" in
            python|python3|python3.*) echo "$pid" ;;
        esac
    done
}

for pid in $(sim_pids); do
    kill "$pid" 2>/dev/null || true
done
sleep 0.3
if [ -n "$(sim_pids)" ]; then
    echo "a simulator is still running; refusing to capture:" >&2
    for pid in $(sim_pids); do
        tr '\0' ' ' < "/proc/$pid/cmdline"; echo
    done >&2
    exit 1
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.ppm "$OUT"/*.png

.venv/bin/python -u tools/daly_sim.py --scenario "$SCEN" >/dev/null 2>&1 &
SIM=$!
trap 'kill $SIM 2>/dev/null || true' EXIT INT TERM
sleep 1

SIM_SCRIPT="$SCRIPT" SIM_SHOT_DIR="$OUT" SIM_QUIT_MS="$QUIT" \
    timeout $(( QUIT / 1000 + 10 )) ./build/sim vcan0 >/dev/null 2>&1 || true

kill $SIM 2>/dev/null || true
wait $SIM 2>/dev/null || true

for f in "$OUT"/*.ppm; do
    convert "$f" -filter point -resize 150% "${f%.ppm}.png"
done
ls "$OUT"/*.png

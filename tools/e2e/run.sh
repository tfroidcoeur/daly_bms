#!/usr/bin/env bash
# End to end, no hardware: the remote-reporting firmware (sim_headless
# --server, same core/ as the board) against the avia-bms plugin in a real
# WordPress, with daly_sim.py playing three Daly packs that fall asleep.
#
#   tools/setup-vcan.sh             once (needs sudo)
#   cmake --build build
#   tools/e2e/run.sh                KEEP=1 leaves WordPress running
#
# The plugin comes from AVIA_BMS_DIR (default: ../php/aviadpc/avia-bms next to
# this repo). Timeouts are shortened: 3 s of live screen, 15 s to collect.
# The plugin refuses check-ins under 60 s; the test runs a copy with that floor
# at 1 s, so it can check in every 2 s.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO/tools/e2e"
PLUGIN_SRC="${AVIA_BMS_DIR:-$(cd "$REPO/../php/aviadpc/avia-bms" && pwd)}"
export E2E_PORT="${E2E_PORT:-8090}"
URL="http://localhost:$E2E_PORT"
API="$URL/wp-json/avia-bms/v1"
IF="${IF:-vcan0}"
WORK="$(mktemp -d)"
chmod 711 "$WORK"   # the containers' www-data reaches the plugin copy in it

export AVIA_BMS_DIR="$WORK/avia-bms"
cp -r "$PLUGIN_SRC" "$AVIA_BMS_DIR"
rm -rf "$AVIA_BMS_DIR/.git"
sed -i -E 's/(const CHECKIN_S_MIN = )[0-9]+;/\11;/' "$AVIA_BMS_DIR/includes/class-settings.php"
grep -q 'const CHECKIN_S_MIN = 1;' "$AVIA_BMS_DIR/includes/class-settings.php" \
    || { echo "could not lower CHECKIN_S_MIN in the plugin copy"; exit 2; }
PY="$REPO/.venv/bin/python"
PASS=0

cd "$HERE"
wp() { docker compose --progress quiet run --rm -T cli "$@"; }
php_eval() { wp eval "$1"; }

cleanup() {
    [ -n "${SIM_PID:-}" ] && kill "$SIM_PID" 2>/dev/null || true
    [ -n "${DALY_PID:-}" ] && kill "$DALY_PID" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    if [ -z "${KEEP:-}" ]; then
        docker compose down -v >/dev/null 2>&1 || true
    fi
    echo "logs: $WORK"
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; echo "--- device log tail:"; tail -30 "$WORK/device.log"; exit 1; }
ok() { PASS=$((PASS + 1)); echo "  ok  $*"; }

# wait_for PATTERN SECONDS [FROM_LINE]: a device log line matching PATTERN,
# after line FROM_LINE.
wait_for() {
    local pat="$1" secs="$2" from="${3:-0}"
    for _ in $(seq 1 $((secs * 10))); do
        if tail -n +"$((from + 1))" "$WORK/device.log" | grep -qE "$pat"; then
            return 0
        fi
        sleep 0.1
    done
    fail "no '$pat' within ${secs}s"
}
mark() { wc -l < "$WORK/device.log"; }

ip link show "$IF" >/dev/null 2>&1 || { echo "no $IF - run tools/setup-vcan.sh first"; exit 2; }
[ -x "$REPO/build/sim_headless" ] || { echo "build first: cmake --build build"; exit 2; }

echo "=== WordPress"
docker compose down -v >/dev/null 2>&1 || true
docker compose up -d db wp >/dev/null
for _ in $(seq 1 90); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$URL/" || true)" != 000 ] && break
    sleep 1
done
wp core install --url="$URL" --title=E2E --admin_user=admin --admin_password=admin \
    --admin_email=admin@example.com --skip-email >/dev/null
wp rewrite structure '/%postname%/' >/dev/null
wp plugin activate avia-bms >/dev/null
php_eval 'var_export( true === Avia_BMS_Settings::set_checkin_s(2) );' | grep -q true \
    || fail "plugin refused a 2 s check-in interval"
php_eval 'Avia_BMS_Settings::set_auto_period_h(0);'
php_eval 'echo Avia_BMS_Settings::rotate_token();' > "$WORK/token"
chmod 600 "$WORK/token"
ok "plugin active, check-in every 2 s, automatic report off"
php_eval 'echo Avia_BMS_Lock::run( function () { return Avia_BMS_Lock::run( function () { return "inner"; }, "held" ); }, "outer" );' | grep -qx held \
    || fail "the lock did not exclude a second taker on MariaDB"
php_eval 'var_export( false === get_option( "avia_bms_lock" ) );' | grep -q true || fail "lock row left behind"
ok "lock: exclusive on the real database, released afterwards"

echo "=== packs and device"
"$PY" "$REPO/tools/daly_sim.py" -i "$IF" --scenario sleepy --sleep-after 5 \
    --wake-requests 3 > "$WORK/daly.log" 2>&1 &
DALY_PID=$!
mkfifo "$WORK/touch"
"$REPO/build/sim_headless" "$IF" --victron --server "$API" \
    --token-file "$WORK/token" --local-timeout 3000 --report-timeout 15000 \
    < "$WORK/touch" > "$WORK/device.log" 2>&1 &
SIM_PID=$!
exec 3> "$WORK/touch"

wait_for 'checkin ok cmd=none next=2s' 10
ok "first check-in accepted"
wait_for 'state LOCAL -> IDLE' 10
wait_for 'sleep [0-9]+s \(awake\)' 5
ok "boot -> live screen -> standby after the local timeout; 2 s interval waited awake"

timeout 3 candump -n 200 "$IF" > "$WORK/bus.txt" || true
REQ=$(grep -cE ' 1890..40 | 189[0-9]0[123]40 ' "$WORK/bus.txt" || true)
[ "$REQ" -eq 0 ] || fail "bus not quiet in standby: $REQ requests to packs"
VIC=$(grep -cE ' 35[56] ' "$WORK/bus.txt" || true)
[ "$VIC" -eq 0 ] || fail "bus not quiet in standby: $VIC Victron frames"
ok "bus quiet in standby"

sleep 6   # past --sleep-after: the packs doze off
grep -q 'asleep' "$WORK/daly.log" || fail "packs did not fall asleep"
ok "packs asleep"

echo "=== manual refresh"
M=$(mark)
php_eval 'var_export( ! is_wp_error( Avia_BMS_Requests::request_manual( time() ) ) );' | grep -q true \
    || fail "manual request refused"
wait_for 'checkin ok cmd=report' 10 "$M"
wait_for 'state IDLE -> REPORT' 5 "$M"
wait_for 'upload 204 .*3/3 packs online' 30 "$M"
wait_for 'state REPORT -> IDLE' 5 "$M"
ok "refresh: picked up, packs woken, snapshot uploaded (204), back to standby"
grep -q 'awake' "$WORK/daly.log" || fail "packs never woke"
curl -s "$API/snapshot" > "$WORK/snapshot.json"
python3 - "$WORK/snapshot.json" <<'PYEOF' || fail "public snapshot wrong"
import json, sys
d = json.load(open(sys.argv[1]))
s = d["snapshot"]
assert s and s["schema"] == 1, "no snapshot"
assert s["bank"]["online"] == 3, s["bank"]
assert all(p["online"] and len(p["cells_mv"]) == 24 for p in s["packs"])
assert d["refresh"] is None, d["refresh"]
assert "ip" not in json.dumps(d["device"])
PYEOF
ok "public /snapshot shows 3 packs, 24 cells each, no refresh pending"

echo "=== automatic report"
M=$(mark)
php_eval 'Avia_BMS_Settings::set_auto_period_h(1); update_option("avia_bms_auto_next", time() - 1);'
wait_for 'checkin ok cmd=report' 10 "$M"
wait_for 'upload 204' 30 "$M"
php_eval 'echo Avia_BMS_Schedule::next() > time() ? "advanced" : "stuck";' | grep -q advanced \
    || fail "schedule did not advance"
php_eval 'Avia_BMS_Settings::set_auto_period_h(0);'
ok "due schedule -> report; next slot moved forward"

echo "=== Victron broadcast while awake"
timeout 6 candump -L "$IF,355:7FF,356:7FF" > "$WORK/victron.txt" || true &
CANDUMP_PID=$!
sleep 0.5
M=$(mark)
echo t >&3
wait_for 'state IDLE -> LOCAL' 3 "$M"
wait_for 'state LOCAL -> IDLE' 15 "$M"
wait "$CANDUMP_PID" || true
python3 - "$WORK/victron.txt" <<'PYEOF' || fail "Victron broadcast wrong"
import struct, sys
frames = {}
for line in open(sys.argv[1]):
    cid, data = line.split()[2].split("#")
    frames.setdefault(int(cid, 16), []).append(bytes.fromhex(data))
assert len(frames.get(0x355, [])) >= 2, frames.keys()
assert len(frames.get(0x356, [])) >= 2, frames.keys()
soc, soh = struct.unpack_from("<HH", frames[0x355][-1])
mv, da = struct.unpack_from("<hh", frames[0x356][-1])
assert 0 <= soc <= 100 and soh == 100, (soc, soh)
assert 7500 <= mv <= 9000, mv      # the three sim packs average about 81.6 V
PYEOF
ok "screen on -> 0x355/0x356 about once a second, sane values; standby -> they stop"

echo "=== touch"
M=$(mark)
echo t >&3
wait_for 'touch' 3 "$M"
wait_for 'state IDLE -> LOCAL' 3 "$M"
wait_for 'checkin ok' 5 "$M"
wait_for 'state LOCAL -> IDLE' 10 "$M"
ok "touch -> live screen and an immediate check-in -> standby"

echo "=== live reports while the screen is on"
php_eval 'var_export( true === Avia_BMS_Settings::set_live_s(2) );' | grep -q true \
    || fail "plugin refused a 2 s live interval"
M=$(mark)
echo t >&3
wait_for 'checkin ok' 5 "$M"
wait_for 'live 204 \(every 2s, 3/3 packs online\)' 20 "$M"
curl -s "$API/snapshot" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["live_every"] == 2, d["live_every"]; assert d["snapshot"]["packs"][0]["online"]' \
    || fail "public snapshot not live"
wait_for 'state LOCAL -> IDLE' 20 "$M"
sleep 16   # past the page's grace for a 2 s interval (15 s)
curl -s "$API/snapshot" | python3 -c 'import json,sys; assert json.load(sys.stdin)["live_every"] is None' \
    || fail "still live after the screen went to standby"
php_eval 'Avia_BMS_Settings::set_live_s(null);'
ok "screen on -> live snapshots (live_s from WP admin) -> page live; standby -> they stop"

echo "=== check-ins disabled"
M=$(mark)
php_eval 'Avia_BMS_Settings::set_checkin_s(0);'
wait_for 'next=never' 10 "$M"
wait_for 'sleep until touch or bus' 5 "$M"
php_eval 'var_export( is_wp_error( Avia_BMS_Requests::request_manual( time() ) ) );' | grep -q true \
    || fail "manual refresh accepted while disabled"
php_eval 'var_export( true === Avia_BMS_Settings::set_checkin_s(2) );' | grep -q true \
    || fail "plugin refused a 2 s check-in interval"
M=$(mark)
echo t >&3
wait_for 'checkin ok cmd=none next=2s' 10 "$M"
ok "disabled: no timed check-ins, refresh refused; a touch picks up the re-enabled interval"

echo "=== server down"
M=$(mark)
docker compose stop wp >/dev/null
wait_for 'checkin failed' 10 "$M"
wait_for 'checkin failed .*next=4s' 10 "$M"
docker compose start wp >/dev/null
wait_for 'checkin ok' 60 "$M"
ok "server down -> failed check-ins with back-off -> recovers"

echo "=== current flowing keeps the screen on"
kill "$DALY_PID" 2>/dev/null || true
wait "$DALY_PID" 2>/dev/null || true
"$PY" "$REPO/tools/daly_sim.py" -i "$IF" --scenario normal > "$WORK/daly-normal.log" 2>&1 &
DALY_PID=$!
M=$(mark)
echo t >&3
wait_for 'state IDLE -> LOCAL' 5 "$M"
sleep 10   # over three times the 3 s local timeout
tail -n +"$((M + 1))" "$WORK/device.log" | grep -q 'state LOCAL -> IDLE' \
    && fail "screen went to standby while the bank was discharging"
kill "$DALY_PID" 2>/dev/null || true
wait "$DALY_PID" 2>/dev/null || true
"$PY" "$REPO/tools/daly_sim.py" -i "$IF" --scenario idle > "$WORK/daly-idle.log" 2>&1 &
DALY_PID=$!
wait_for 'state LOCAL -> IDLE' 15 "$M"
ok "discharging keeps the screen on past its timeout; at rest it goes to standby"

echo "=== a pack that does not answer"
kill "$DALY_PID" 2>/dev/null || true
wait "$DALY_PID" 2>/dev/null || true
"$PY" "$REPO/tools/daly_sim.py" -i "$IF" --scenario offline > "$WORK/daly-offline.log" 2>&1 &
DALY_PID=$!
sleep 16   # the scenario silences pack 3 from 15 s to 35 s after start
M=$(mark)
php_eval 'sleep(1); var_export( ! is_wp_error( Avia_BMS_Requests::request_manual( time() + 700 ) ) );' | grep -q true \
    || fail "manual request refused"
wait_for 'checkin ok cmd=report' 10 "$M"
wait_for 'upload 204 .*2/3 packs online' 30 "$M"
curl -s "$API/snapshot" | python3 -c '
import json, sys
d = json.load(sys.stdin)["snapshot"]
assert d["packs"][2] == {"addr": 3, "online": False}, d["packs"][2]
assert any(w["code"] == "pack_offline" and w["pack"] == 3 for w in d["warnings"])
' || fail "offline pack not reported as offline"
ok "silent pack: uploaded after the collect timeout, marked offline, with a pack_offline alarm"

echo "=== a bad token is refused"
code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $(printf '0%.0s' {1..64})" "$API/device/checkin")
[ "$code" = 401 ] || fail "wrong token got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$API/device/checkin")
[ "$code" = 401 ] || fail "no token got $code"
ok "wrong or missing token -> 401"

echo ""
echo "=== $PASS checks passed"

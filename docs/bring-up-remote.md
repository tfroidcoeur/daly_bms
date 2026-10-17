# Bring-up: remote reporting

Checks for the first flash of the remote-reporting firmware (1.1.0) on the
real board, in order. Everything here has already run on the laptop
(`tools/e2e/run.sh`: the same `core/` against a real WordPress and simulated
packs that fall asleep). What is left is what only the hardware can show.

Work on the bench, or through a USB isolator: with USB-C to a laptop, the
laptop's ground is tied to pack negative (see
[hardware/wiring.md](hardware/wiring.md)).

## 0. Hardware

1. **Rs rework.** On the SN65HVD230 breakout, lift the resistor from Rs (pin 8)
   to GND and run a wire from Rs to **P1 pin 15 (GPIO18)**. Until this is done,
   build with `CONFIG_BMS_CAN_RS_GPIO=-1` (menuconfig → Remote reporting).
   With the wire fitted, the KEY button is no longer read - only the pads.
   Don't press KEY while the firmware drives GPIO18: the button shorts it.
2. **Power LED** on the breakout, if it has one: remove it (1-3 mA, always).
3. **Power bank**: it must have a low-current / always-on mode. An ordinary
   bank switches itself off within minutes at a few mA and stays off.

## 1. Flash and configure

```bash
cd port/esp32
idf.py menuconfig    # Daly BMS monitor → Remote reporting: check CAN Rs GPIO
idf.py flash monitor
```

The partition table changes to the 1.5 MB single-app layout (Wi-Fi and TLS
need the room); NVS stays where it was.

**Open the console:** press reset (or plug in), wait for the boot log, then
touch a pad within 10 seconds of the reset - the log says `console open for
300 s`. (Not held through the reset: the pads calibrate at boot and would take
the finger for the baseline.) Then:

```
bms> wifi <ssid> <password>
bms> token <the 64-character token from WP admin → Settings → Avia BMS>
bms> status
```

Reset the board so it starts the network with the new settings, open the
console again the same way, and:

```
bms> status        (network: running)
bms> checkin
```

`status` shows the password and token only as *set* / *not set*. The console
closes after 5 minutes. While it is open the board does not deep-sleep.

The log also prints, once per boot, how much of the network task's stack was
never used (`net task stack: N bytes never used`) - if that is under ~1000
after a real HTTPS check-in, say so.

## 2. Checks

| # | Do | Expect |
|---|----|--------|
| 1 | `checkin` on the console | log `check-in ok, none, next in 900 s`; WP admin shows the check-in time, firmware 1.1.0 |
| 2 | Press Refresh on the page (signed in) | within one interval: log `check-in ok, report`, the bus comes alive, `upload: 204`; the page shows the snapshot with a fresh time |
| 3 | Leave it alone 5 min | "Zzz" on the screen, log `deep sleep`, no CAN requests (`candump can0` on the bench adapter stays silent) |
| 4 | Touch a pad | live screen within a second or two; the first touch does not change page |
| 5 | Let a dashboard poll address 0x10 (or `cansend can0 18901040#0000000000000000` a few times) | the board wakes, screen live, the virtual BMS answers after the first full poll round |
| 6 | Does "Zzz" stay on the glass through deep sleep? | yes - if the screen goes blank or grey instead, report it: the panel pins' hold needs another look |
| 7 | **CAN wake of sleeping packs** (the one real unknown) | leave the bank idle > 1 h with the board asleep, check in the Daly app that the packs sleep, then Refresh: the packs must answer within the 60 s collection window and the snapshot show 3/3 online |

If step 7 fails - the packs stay silent on CAN while asleep - the snapshot
still arrives, with the packs marked offline. Then either enable CAN wake-up in
Daly's BmsMonitor (WakeUpMethod) or raise the packs' sleep time.

## 3. Battery life

Nothing has been measured. The design target is three months on a 20 Ah bank,
an average of ≤ 5.8 mA. Note the bank's charge when it goes in and check it
after a week; then tune the check-in interval in WP admin (the only knob):
longer is cheaper, every 15 min is the default.

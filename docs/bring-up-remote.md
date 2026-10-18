# Bring-up: remote reporting

Checks for the first flash of the remote-reporting firmware (1.1.0) on the
real board, in order. Everything here has already run on the laptop
(`tools/e2e/run.sh`: the same `core/` against a real WordPress and simulated
packs that fall asleep). What is left is what only the hardware can show.

Work on the bench, or through a USB isolator: with USB-C to a laptop, the
laptop's ground is tied to pack negative (see
[hardware/wiring.md](hardware/wiring.md)).

## 0. Hardware

1. **Rs rework.** GPIO18 drives the transceiver's Rs pin: low while awake,
   high (standby, ~0.4 mA) in deep sleep. Until this is done, build with
   `CONFIG_BMS_CAN_RS_GPIO=-1` (menuconfig → Remote reporting).

   **The breakout's two resistors:**

   | Marking | Value | What it is |
   |---|---|---|
   | `121` | 120 Ω | CAN termination, CANH-CANL. **Leave it** (termination rules: [hardware/sn65hvd230.md](hardware/sn65hvd230.md)). |
   | `01C` | 10 kΩ (EIA-96) | Rs (pin 8) to GND: slope-control mode. **This one is reworked.** |

   **Find its GND end.** Board unpowered, meter on continuity: the end of
   `01C` that beeps to GND is the GND end; the other end reads ~10 kΩ to GND
   and beeps to pin 8 (Rs; pin 1 is at the dot, pin 8 directly across).

   **Lift the GND end, keep the resistor.** Heat the GND end and lift it with
   tweezers, so the resistor stands on its Rs end. Put Kapton tape or glue
   under the raised end so it cannot touch the GND pad again. Leave the GND pad
   empty. The 10 kΩ now sits between Rs and the wire, so awake the transceiver
   stays in slope-control mode, as before; in deep sleep the pin pulls Rs to
   ~3.3 V, well over the 0.75 × VCC standby threshold.

   **Wire the raised end to GPIO18 at the KEY button**, not at the header. The
   KEY button (top left on the front) switches GPIO18 to GND, so one of its
   pads is GPIO18: the one that beeps to the **GP18** socket of the header (top
   of the right-hand column, next to SCL, above GP17). Solder the wire there and
   to the raised end of `01C`; strain-relieve both ends with hot glue. The
   header's own solder joints are on the back, under the glued-on screen, and a
   pin pushed into a socket is hard to solder. Never use **GP19/GP20**: they
   are the USB data lines (flashing and the console).

   **Measure before powering up:**
   - KEY pad (GPIO18) to GND, SCL and GP17: no beep.
   - KEY pad to transceiver pin 8: ~10 kΩ.
   - Transceiver pin 8 to GND: no beep (the lifted end is clear).
   - CANH to CANL: ~120 Ω, unchanged.

   With the wire fitted, the firmware no longer reads KEY - only the pads.
   **Don't press KEY while the board is asleep:** GPIO18 is then driven high
   and the button shorts it to GND. Pressing it while awake (GPIO18 low) does
   nothing.

   Other header wires (CAN on GP17/GP3, the pads on GP1/GP2) that sit loosely
   in the female header: solder them to a piece of male header strip on the
   bench first and push that in, or glue the Dupont plugs in place. A CAN wire
   that drops out looks like a pack going offline.
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
| 6a | Touch a pad with the page open | the page shows **Live** and updates every ~5 s; the screen's top right shows filled signal bars. Walk out of Wi-Fi range: hollow bars, the screen keeps working, back in range the page goes live again |
| 6b | With current flowing (winch, charger) | the screen does not go to "Zzz" after 5 min; at rest it does |
| 7 | **CAN wake of sleeping packs** (the one real unknown) | leave the bank idle > 1 h with the board asleep, check in the Daly app that the packs sleep, then Refresh: the packs must answer within the 60 s collection window and the snapshot show 3/3 online |

If step 7 fails - the packs stay silent on CAN while asleep - the snapshot
still arrives, with the packs marked offline. Then either enable CAN wake-up in
Daly's BmsMonitor (WakeUpMethod) or raise the packs' sleep time.

## 3. Battery life

Nothing has been measured. The design target is three months on a 20 Ah bank,
an average of ≤ 5.8 mA. Note the bank's charge when it goes in and check it
after a week; then tune the check-in interval in WP admin (the only knob):
longer is cheaper, every 15 min is the default.

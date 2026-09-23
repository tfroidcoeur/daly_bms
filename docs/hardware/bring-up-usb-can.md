# Bench bring-up: laptop + USB-CAN adapter + three Daly packs

The host simulator and the firmware share `core/` and `ui/` verbatim, so a
USB-CAN adapter on the laptop exercises the same decoder, the same poll
schedule and the same screens the ESP32 will run - with a two-second rebuild
instead of a flash cycle. Do the protocol validation here. Move to the board
only once the numbers are known good.

This is the laptop variant of [wiring.md](wiring.md#bring-up-order) steps 2-5.
No ESP32 and no SN65HVD230 are involved: the adapter has its own transceiver.

---

## 0. Before anything is connected

**Re-address the packs.** All three Daly units ship as board number `0x01`.
Bussed together as shipped, all three answer every request and pack 1 shows
values flickering between three different batteries. Set them to `0x01`, `0x02`,
`0x03` with Daly's Windows **BmsMonitor** tool over each unit's UART/USB port -
not over CAN, and not with the phone app, which does not set this reliably.
Do this one unit at a time, off the bus.

**Decide how the laptop is powered.** The adapter ties laptop ground to pack
negative. On a live bank with the laptop also on mains, that is a ground loop
through your CAN ground wire and a plausible route to a dead laptop. Run the
laptop **on battery, unplugged from mains**, or put a USB isolator in front of
the adapter. Bench first, bank later.

---

## 1. Wire and terminate

```
  laptop ── USB ── adapter ──┬── CANH/CANL twisted pair ──┬── Daly 0x01
                             │   + ground wire alongside  ├── Daly 0x02
                          [120R]                          └── Daly 0x03
                        one end                                    │
                                                                [120R]
                                                              other end
```

- **Exactly two 120 Ω terminators**, at the two physical ends and nowhere else.
  The adapter is one end. The last Daly in the chain is the other.
- Adapters usually carry an onboard 120 Ω, sometimes on a jumper. Daly units may
  terminate internally. **Measure before powering**: CANH-CANL across the
  assembled, unpowered bus should read about **60 Ω**. 120 Ω means one
  terminator; 40 Ω means three.
- Twisted pair for CANH/CANL with a ground wire alongside. Stubs under ~30 cm.

### Adapter connector pinout

IXXAT USB-to-CAN V2 ships with either a sub-D9 or an RJ45, both **to CiA 303-1**
(the datasheet says so explicitly). RJ45 variants include an RJ45-to-sub-D9
adapter cable in the box.

| RJ45 | Signal | T568B colour | | Sub-D9 | Signal |
|---|---|---|---|---|---|
| 1 | **CAN_H** | white/orange | | 7 | **CAN_H** |
| 2 | **CAN_L** | orange | | 2 | **CAN_L** |
| 3 | CAN_GND | white/green | | 3 | CAN_GND |
| 7 | CAN_GND | white/brown | | | |
| 8 | CAN_V+ (optional supply) | brown | | 9 | CAN_V+ (optional) |
| 4, 5 | reserved - leave unconnected | | | | |
| 6 | CAN_SHLD (optional) | green | | 5 | CAN_SHLD |

Two things bite here. **CAN_H is pin 1 and CAN_L is pin 2** - the opposite order
from the sub-D9, where CAN_L is the lower pin number. And pins 1-2 are the orange
pair in T568B, so a standard Ethernet patch cable already gives CANH/CANL a
twisted pair, which is what you want; cut one open rather than crimping fresh.

**Leave pin 8 unconnected.** It is an optional bus supply rail, not ground.

## 2. Bring up the interface

250 kbit/s, extended frames. Which command depends on the adapter:

```bash
# Native gs_usb / candleLight / PCAN - shows up as a netdev on its own
sudo ip link set can0 type can bitrate 250000
sudo ip link set up can0

# slcan adapters (CANable in slcan firmware, USBtin) - -s5 is 250 kbit/s
sudo slcand -o -c -s5 /dev/ttyACM0 can0
sudo ip link set up can0
```

Confirm it is up and counting:

```bash
ip -details -statistics link show can0
```

## 3. Build

```bash
cmake -B build && cmake --build build
ctest --test-dir build          # 711 checks, must be clean before you start
```

## 4. First contact - one pack, raw

Connect **one** Daly and start with the raw logger:

```bash
./build/sim_headless can0 --raw
```

`--raw` drops the kernel filter and prints every frame on the bus with the decode
alongside it, so the bytes and their interpretation compare on one line:

```
     162  TX 18930140  ->  0x93 mos           pack 0x01
     164  RX 18934001  [8] 02 01 01 13 00 03 96 3F   pack 1  0x93 mos  state 2  chg ON  dsg ON  235071 mAh
     202  TX 18950140  ->  0x95 cell volts    pack 0x01
     204  RX 18954001  [8] 01 0C FD 0D 03 0D 04 00   pack 1  0x95 cell volts  frame 1: 3325 3331 3332 mV
```

Expect requests as `18900140` and replies as `18904001`.

Frames that do not decode are printed too, with the reason - `standard ID, not
Daly`, `not addressed to host 0x40`, `from unknown pack 0x05`. That is the point
of dropping the filter: through it, a pack answering in a format we did not
predict shows **nothing at all**, which is indistinguishable from a pack that is
dead. `candump -x -t d can0` in a second terminal is still useful below the frame
level, but it is no longer needed to see unexpected traffic.

**If it is silent**, check in this order:

| Symptom | Look at |
|---|---|
| `candump` shows nothing, TX errors climbing in `ip -s link` | termination, bit rate, CANH/CANL swapped |
| `candump` shows our requests only | pack not powered, wrong address, pack's CAN port disabled in BmsMonitor |
| `candump` shows replies, `sim_headless` blank | ID format differs from our assumption - read the raw IDs |

Both views count transmit failures and say so, because that is what a broken
bench setup shows first - with nothing on the bus to ACK, every write fails:

```
link   tx 0 ok, 214 FAILED - nothing is ACKing: check termination, bit rate, CANH/CANL
```

The kernel's own counters in `ip -details -statistics link show can0` are the
next place to look, and the only place that separates the causes.

## 5. Validate the decode table - the actual point of this session

**The field layouts in [daly-can-protocol.md](daly-can-protocol.md) are community
reverse-engineering, not a Daly datasheet.** Run Daly's own app against the same
pack and compare, field by field, against the annotated `--raw` output - the raw
bytes and our reading of them sit on the same line, which is what makes the
comparison quick.

`0x97` balance and `0x98` faults print as bytes with no interpretation, on
purpose: their bit meanings have never been confirmed, and naming them would be
inventing detail.

The three most likely to be wrong, because all three are biased or ordered
rather than plain:

| Check | Where | Symptom if wrong |
|---|---|---|
| current, 30000 offset | `core/daly_proto.h:60` | reads ~3000 A at rest, or sign inverted |
| temperature, 40 offset | `core/daly_proto.h:66` | reads ~-40 C or ~+40 C off |
| `0x95` cell ordering | `core/daly_proto.c:67` | cell count right, individual values shuffled |

Fix `core/daly_proto.c`, add a case to `tests/test_core.c` with the real bytes
you captured, rebuild, re-check. **Do not move on until every number matches.**

## 6. All three packs

Bus all three. Within one poll round (~1.1 s) all three should report:

```bash
./build/sim_headless can0       # expect "bank 3/3 online"
```

The `link` line at the foot of that view carries the running tx/rx counts.

Then pull one pack's connector. Exactly that pack must go `NO DATA` within 5 s,
and the other two must be unaffected. If two go dark, they are sharing an
address - go back to step 0.

## 7. The GUI

```bash
./build/sim can0
```

Same screens the panel will show, at 400x300. `n` = short KEY press (cycle
pages), `N` = long press (cell detail). This is where you find out whether the
layout survives real values rather than the simulator's tidy ones.

## 8. On the live bank

Laptop on battery or behind an isolator. Cross-check SoC, pack voltage and
current per pack against Daly's app. Watch a real winch pull: current should
track, and the gauge is scaled 0-1000 A magnitude-only.

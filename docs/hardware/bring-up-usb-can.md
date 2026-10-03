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

**IXXAT USB-to-CAN V2** (the reference adapter) has no mainline driver.
`tools/setup-ixxat.sh` clones HMS's SocketCAN driver, builds and installs it
through DKMS, and brings `can0` up at 250 kbit/s. Re-run it after a kernel
upgrade.

```bash
sudo ./tools/setup-ixxat.sh        # optional bitrate argument, default 250000
```

With Secure Boot on, the first attempt fails with *Loading of module with
unavailable key is rejected*: the DKMS signing key is not trusted yet. Enrol it
once, then re-run the script:

```bash
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der   # set a one-time password
sudo reboot
```

At the blue MOK Manager screen: *Enroll MOK* -> *Continue* -> *Yes* -> the
password -> *Reboot*. Later kernel updates are signed with the same key.

Confirm it is up and counting:

```bash
ip -details -statistics link show can0
```

## 3. Build

```bash
cmake -B build && cmake --build build
ctest --test-dir build          # must be clean before you start
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

[daly-can-protocol.md](daly-can-protocol.md) now carries
**Daly's own protocol document V1.0** alongside the community
reverse-engineering this project was built from. They agree on `0x90`, `0x91`,
`0x92`, `0x93` and `0x97`, which is strong evidence but not the same thing as
having seen it work.
**They disagree in two places, and this session is where a real pack settles both**
- see [the two open questions](#the-two-open-questions) below.

Nothing on the display is trustworthy until the layouts are checked against a
pack.

Check them against **physical reality first, and Daly's app second**. A
multimeter and a pack you can look at validate more than a second piece of
software does, because two decoders can share the same wrong assumption and
agree with each other all day.

A single pack fresh from the factory answers on `0x01`, so packs 2 and 3 will
read `NO DATA` throughout this step. That is correct, not a fault.

### The five checks

Each targets one assumption, and each fails distinctively:

| Check | Against | If wrong |
|---|---|---|
| Pack voltage | a multimeter across the terminals | scaling - we assume 0.1 V units |
| **Current at rest** | should be ~0 A with nothing connected | **~±3000 A means the 30000 offset is wrong** (`core/daly_proto.h:117`) |
| **Temperature** | a room thermometer | **~40 C out either way means the 40 offset is wrong** (`core/daly_proto.h:123`) |
| **Sum of the cell voltages** | should equal the measured pack voltage | `0x95` ordering, scaling and multi-frame reassembly, all in one number |
| Cell count | the pack in front of you | `0x94` - and this one gates the others |

**The sum is the strongest single test.** Twenty-four cells at ~3.3 V adding up
to the measured pack voltage means byte order, scaling, frame indexing and the
commit rule are all correct together. A correct sum with individual cells
shuffled means the ordering within a frame is wrong.

**Cell count gates everything.** The decoder refuses to read cells until `0x94`
has said how many there are, so a wrong cell-count field gives *no* cell data
rather than wrong cell data. Voltage and current arriving but no cells ever
appearing points at `0x94` - or at the frame numbering, which is the next section.

### The two open questions

Both are answered by looking, not by reasoning, and both take seconds.

**1. Does `0x95` number its frames from 0 or from 1?** Daly's document says 0, the
community layout says 1, and every driver written against real hardware says 1 -
so expect `01`. The decoder works it out from the traffic rather than assuming. In the `--raw` output, read byte 0 of the first `0x95` reply after a
request:

```
    4894  RX 18954001  [8] 01 0C FD 0D 03 0D 04 00   pack 1  0x95 cell volts    frame 1: 3325 3331 3332 mV
                           ^^ this byte
```

A burst that starts at `00` follows the document; one that starts at `01`
follows the community layout. Either is handled, so
**this is a note to make, not a fault to fix** - but note it, because it tells
you how long the cells take to appear. A 1-based pack spends its first burst
being identified, so cells show up on the second round, about a second later
than the other fields.

If cells never appear at all and `0x94` is reporting the right count, the frame
numbering is where to look: the raw log shows exactly which byte 0 values arrived.

**2. Does `0x94` put a cycle count in bytes 5-6, or are they reserved?** Daly's
document says reserved; every driver I could find reads a cycle count there, so
expect one. Read the tail of a `0x94` reply:

```
     731  RX 18944001  [8] 18 04 00 01 00 00 85 00   pack 1  0x94 status        24 cells  4 sensors  b5-7 00 85 00
                                          ^^ ^^ ^^
```

That pack fills them: `0x0085` is 133, and its `CAPACITY` field will read
`240.0 Ah 133 cy`. Check the number against Daly's app before believing it - a
plausible-looking value could equally be some other field entirely.

All three bytes zero means the document is right for this firmware, and
`CAPACITY` will show Ah alone rather than inventing a cycle count.

### Then put a load on it

Anything - a lamp, a small motor. **Current must go negative.** Positive is
charging by our convention.

This is the one check here with a known way to fail: the Daly drivers in
circulation disagree about which way the current points (see
[daly-can-protocol.md](daly-can-protocol.md#0x90---pack-voltage-current-soc)).
If it reads positive under load, nothing is broken - enable
`CONFIG_BMS_INVERT_CURRENT` in `idf.py menuconfig` (or rebuild the host with
`-DBMS_INVERT_CURRENT=ON`) and check again. `daly_sim.py --invert-current`
reproduces the other convention on the bench.

The overview and the charging-below-freezing alarm do not depend on this
either way; they read the BMS's own charging state from `0x93`.

### What cannot be checked this way

`0x98` faults are named on screen from Daly's table (see
[daly-can-protocol.md](daly-can-protocol.md#0x98---failure--alarm-flags)), which
dbus-serialbattery corroborates - but no bit has yet been seen set on this
project's hardware. If you can provoke a real one (a charge attempt below the
pack's charge-temperature limit is the gentlest), check that the name on the pack
page matches what Daly's app says, and keep the raw line from `--raw`.

SoC has no physical referent either, so Daly's own app is genuinely the only
cross-check for it.

### When something is wrong

Fix `core/daly_proto.c`, then
**add a case to `tests/test_core.c` using the real bytes you captured**, so the
correction is pinned by a test rather than by memory. Rebuild, re-check. Do not
move on until every number matches.

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
track, and the gauge is scaled 0-350 A (`UI_GAUGE_MAX_A`), magnitude only.

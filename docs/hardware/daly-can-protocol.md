# Daly smart BMS - CAN protocol

## Status of this document

Daly publish an overview of their three protocols (CAN / UART / RS485) but not a
full field-level datasheet. The command table below is assembled from Daly's own
protocol note plus community reverse-engineering (dbus-serialbattery, the DIY Solar
Power Forum threads).

**Treat the field layouts as unverified until checked against the real hardware.**
The bring-up procedure in [wiring.md](wiring.md) flashes a raw frame logger first
for exactly this reason. Do not put a number on screen that has not been
cross-checked against Daly's own app.

## Physical layer

| | |
|---|---|
| Bit rate | **250 kbit/s** |
| Frame format | **29-bit extended** identifiers |
| Data length | 8 bytes, always |

(A few customised Daly units use standard 11-bit frames instead. If nothing
answers on extended IDs, that is the first thing to check.)

## Identifier layout

The BMS address is a *field inside the identifier*, which is how several BMS units
share one bus:

```
Request  (host -> BMS):   0x18 | cmd | bms_addr | host_addr
Response (BMS -> host):   0x18 | cmd | host_addr | bms_addr
                          ^^^^   ^^^   ^^^^^^^^   ^^^^^^^^^
                          prio   cmd   dest       source
```

- `0x18` is the priority nibble pair, constant.
- `host_addr` is **`0x40`** by convention (the "PC software" address).
- `bms_addr` defaults to **`0x01`**.

Worked example, asking BMS #1 for SoC / voltage / current:

```
  request  ID 0x18900140    (cmd 0x90, to BMS 0x01, from host 0x40)
  response ID 0x18904001    (cmd 0x90, to host 0x40, from BMS 0x01)
```

### Three BMS units on one bus

Each unit must have a distinct address, set with Daly's Windows **BmsMonitor**
tool. The *Smart BMS* phone app does not set the board number reliably.

| Unit | Address | Request ID for cmd 0x90 | Response ID |
|---|---|---|---|
| Pack 1 | `0x01` | `0x18900140` | `0x18904001` |
| Pack 2 | `0x02` | `0x18900240` | `0x18904002` |
| Pack 3 | `0x03` | `0x18900340` | `0x18904003` |

Leave `0x04`, `0x05` free for future packs. Two units sharing an address will
collide continuously and neither will be readable.

## Command table

All payloads are 8 bytes, big-endian.

### `0x90` - pack voltage, current, SoC

| Bytes | Field |
|---|---|
| 0-1 | Cumulative total voltage, u16, **0.1 V** |
| 2-3 | Gather total voltage, u16, 0.1 V (often unused) |
| 4-5 | Current, u16, **0.1 A, offset 30000** -> `A = (raw - 30000) / 10` |
| 6-7 | SoC, u16, **0.1 %** |

Sign convention: after removing the offset, positive is charge, negative is
discharge.

### `0x91` - cell voltage extremes

| Bytes | Field |
|---|---|
| 0-1 | Max cell voltage, u16, mV |
| 2 | Cell number of the max (1-based) |
| 3-4 | Min cell voltage, u16, mV |
| 5 | Cell number of the min (1-based) |
| 6-7 | unused |

### `0x92` - temperature extremes

| Bytes | Field |
|---|---|
| 0 | Max temperature, u8, **offset 40** -> `degC = raw - 40` |
| 1 | Sensor number of the max |
| 2 | Min temperature, u8, offset 40 |
| 3 | Sensor number of the min |
| 4-7 | unused |

### `0x93` - MOSFET state and remaining capacity

| Bytes | Field |
|---|---|
| 0 | State: 0 = stationary, 1 = charging, 2 = discharging |
| 1 | Charge MOSFET on/off |
| 2 | Discharge MOSFET on/off |
| 3 | BMS life cycle counter (increments each frame, useful as a liveness check) |
| 4-7 | Remaining capacity, u32, mAh |

### `0x94` - configuration and status

| Bytes | Field |
|---|---|
| 0 | Number of cells |
| 1 | Number of temperature sensors |
| 2 | Charger status (0/1) |
| 3 | Load status (0/1) |
| 4 | DI/DO bitflags |
| 5-6 | Cycle count, u16 |
| 7 | unused |

Poll this once at startup: it tells you how many cells and sensors to expect, and
therefore how many `0x95` / `0x96` frames will arrive.

### `0x95` - individual cell voltages (multi-frame)

One request produces `ceil(cell_count / 3)` response frames:

| Bytes | Field |
|---|---|
| 0 | Frame index, **1-based** |
| 1-2 | Cell voltage, u16, mV |
| 3-4 | Cell voltage, u16, mV |
| 5-6 | Cell voltage, u16, mV |
| 7 | unused |

Cell index = `(frame_index - 1) * 3 + slot`. Trailing slots in the last frame are
padding - ignore anything past `cell_count`.

The parser must tolerate out-of-order and dropped frames: track which indices have
arrived and only commit a complete set.

### `0x96` - individual cell temperatures (multi-frame)

| Bytes | Field |
|---|---|
| 0 | Frame index, 1-based |
| 1-7 | 7 temperatures, u8 each, offset 40 |

### `0x97` - cell balancing state

64 bits of balance flags, one per cell, LSB = cell 1.

### `0x98` - failure / alarm flags

7 bytes of bitflags covering cell over/under voltage, pack over/under voltage,
charge and discharge overcurrent, temperature alarms, SoC alarms, and various
hardware faults. Byte 7 is a fault-count / summary byte.

Decode conservatively: surface "an alarm is set" plus the raw bytes, and only claim
a specific named fault once it has been confirmed against real hardware.

## Polling strategy

A full round for one pack is `0x90`, `0x91`, `0x92`, `0x93`, `0x95`, and less often
`0x94`, `0x96`, `0x97`, `0x98`.

Poll one pack at a time, round-robin across the three, spacing requests so replies
(especially the multi-frame `0x95`) do not overlap between packs. At 250 kbit/s a
full round for three packs comfortably fits in about a second, which matches the
display's refresh budget anyway.

A pack that misses several consecutive rounds is marked offline and drawn as
`NO DATA` rather than showing stale values.

## Sources

- <https://www.dalybms.com/news/daly-three-communication-protocols-explanation/>
- <https://github.com/Louisvdw/dbus-serialbattery/discussions/561>
- <https://diysolarforum.com/threads/decoding-the-daly-smartbms-protocol.21898/page-2>
- <https://diysolarforum.com/threads/daly-bms-can-and-arduino.22268/>

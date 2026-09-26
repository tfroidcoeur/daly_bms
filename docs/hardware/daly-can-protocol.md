# Daly smart BMS - CAN protocol

## Status of this document

Two sources sit behind this page, and they do not entirely agree.

1. **Daly's own "Daly CAN Communications Protocol V1.0"** (Dongguan Daly
   Electronics, initial version 2019-06-11). Field-level, authoritative, and the
   basis of every table below.
2. **Community reverse-engineering** - dbus-serialbattery, the DIY Solar Power
   Forum threads - which is what this project was originally built from.

They match byte for byte on `0x90`, `0x91`, `0x92`, `0x93` and `0x97`, including
both biased fields (current at 30000 offset, temperature at 40 offset) and the
identifier layout. Two places differ, and both are called out in the tables:

| | Daly V1.0 | Community layout | What the code does |
|---|---|---|---|
| `0x95` / `0x96` byte 0 | frame number **from 0** | frame index **from 1** | learns which, per pack |
| `0x94` bytes 5-7 | **reserved** | cycle count in 5-6 | reads it, flags whether it was there |

Neither disagreement is a rounding error. Pick the wrong frame base and the
decisive frame of every burst is discarded, so the set never completes and **no
cell data is ever published at all**. `core/daly_proto.c` therefore does not
choose: it works the base out from the traffic. See
[the frame numbering section](#frame-numbering-0-based-or-1-based) below.

**The `0x98` fault bits are now fully specified** (Daly V1.0), but this project
still surfaces them as raw bytes - see the note under that command.

Everything here still wants checking against real hardware. The raw frame logger
(`CONFIG_BMS_RAW_LOGGER`, and `--raw` on the host simulator) exists for that, and
[bring-up-usb-can.md](bring-up-usb-can.md) says what to compare against what.

## Physical layer

| | |
|---|---|
| Bit rate | **250 kbit/s** (Daly V1.0 §1.1) |
| Frame format | **29-bit extended** identifiers |
| Data length | 8 bytes, always |

(A few customised Daly units use standard 11-bit frames instead. If nothing
answers on extended IDs, that is the first thing to check.)

## Identifier layout

The BMS address is a *field inside the identifier*, which is how several BMS units
share one bus. Daly V1.0 §2.3 writes it as "Priority + Data ID + BMS Address + PC
Address", with the two addresses swapped in the reply:

```
Request  (host -> BMS):   0x18 | cmd | bms_addr | host_addr
Response (BMS -> host):   0x18 | cmd | host_addr | bms_addr
                          ^^^^   ^^^   ^^^^^^^^   ^^^^^^^^^
                          prio   cmd   dest       source
```

- `0x18` is the priority nibble pair, constant.
- `host_addr` is **`0x40`**, which Daly call the "upper computer".
- `bms_addr` defaults to **`0x01`**, the "BMS master".

The document's own worked examples are `0x18100140` outbound and `0x18104001`
inbound, i.e. data ID `0x10` in place of a real command.

Other addresses Daly reserve, worth knowing so they are not mistaken for a pack:
Bluetooth app `0x80`, GPRS `0x20`.

Worked example, asking BMS #1 for SoC / voltage / current:

```
  request  ID 0x18900140    (cmd 0x90, to BMS 0x01, from host 0x40)
  response ID 0x18904001    (cmd 0x90, to host 0x40, from BMS 0x01)
```

Every request carries **8 bytes of zero** - Daly's table gives the send direction
of every command as "Byte0~Byte7: Reserved".

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
| 3 | BMS life, 0-255, increments each frame - useful as a liveness check |
| 4-7 | Remaining capacity, u32, mAh |

### `0x94` - configuration and status

| Bytes | Field |
|---|---|
| 0 | Number of cells ("No of battery string") |
| 1 | Number of temperature sensors |
| 2 | Charger status: 0 = disconnected, 1 = connected |
| 3 | Load status: 0 = disconnected, 1 = connected |
| 4 | DI/DO bits: 0-3 = DI1-DI4, 4-7 = DO1-DO4 |
| 5-7 | **Reserved per Daly V1.0**; cycle count in 5-6 per the community layout |

Poll this once at startup: it tells you how many cells and sensors to expect, and
therefore how many `0x95` / `0x96` frames will arrive.

Bytes 5-6 are the disputed field. `core/daly_proto.c` reads them as a u16 cycle
count but sets `cycles_valid` only if there was something there, so a pack that
really does reserve them shows no cycle count rather than a plausible zero. **The
pack detail page is the readout for this**: a `CAPACITY` field reading
`240.0 Ah 133 cy` means the firmware fills those bytes, and one reading
`240.0 Ah` alone means it follows the document.

### `0x95` - individual cell voltages (multi-frame)

One request produces `ceil(cell_count / 3)` response frames - up to 16 frames for
48 cells, which is where Daly's "maximum 96 byte, sent in 16 frames" comes from.

| Bytes | Field |
|---|---|
| 0 | Frame number - see below. `0xFF` means the frame is invalid |
| 1-2 | Cell voltage, u16, mV |
| 3-4 | Cell voltage, u16, mV |
| 5-6 | Cell voltage, u16, mV |
| 7 | Reserved |

Cell index = `position_in_burst * 3 + slot`. Trailing slots in the last frame are
padding - ignore anything past `cell_count`.

The parser must tolerate out-of-order and dropped frames: track which positions
have arrived and only commit a complete set.

### `0x96` - individual cell temperatures (multi-frame)

`ceil(temp_count / 7)` frames - Daly's "maximum 21 byte, send in 3 frames" for 16
sensors.

| Bytes | Field |
|---|---|
| 0 | Frame number - see below |
| 1-7 | 7 temperatures, u8 each, offset 40 |

### Frame numbering: 0-based or 1-based?

Daly V1.0 is unambiguous about its own intent:

> `0x95` - Byte0: frame number, starting from 0, 0xFF invalid
> `0x96` - Byte0: frame number, starting at 0

The community layout this project was built from says the index is 1-based, and
firmware in service is widely reported to behave that way. Nothing inside a frame
says which you are talking to, and getting it wrong publishes nothing at all
rather than publishing something wrong.

So `core/daly_proto.c` learns it, per pack and per command, from the only two
observations that can come from one scheme and not the other:

| Observation | Conclusion |
|---|---|
| byte 0 == 0 | 0-based; a 1-based burst never sends this |
| byte 0 == frame count | 1-based; a 0-based burst stops one short of its own count |

Anything in between is consistent with both and is discarded while the question
is open, because a frame that cannot be placed must not be stored. A 0-based pack
settles it on the first frame of its first burst and loses nothing. A 1-based pack
settles it on the last frame, so its cells appear one polling round later - about
a second, once, at startup.

`tools/daly_sim.py --frame-base 0` makes the simulator speak the documented
scheme; the default is 1. Both are covered by the `0x95/0x96 frame numbering`
unit test.

### `0x97` - cell balancing state

Bit 0 = cell 1 balancing, through to bit 47 = cell 48. Bits 48-63 reserved.
0 = closed, 1 = open.

### `0x98` - failure / alarm flags

Fully specified by Daly V1.0. Every bit is 0 = no error, 1 = error.

| Byte | Bit 0 | Bit 1 | Bit 2 | Bit 3 | Bit 4 | Bit 5 | Bit 6 | Bit 7 |
|---|---|---|---|---|---|---|---|---|
| 0 | cell volt high L1 | cell volt high L2 | cell volt low L1 | cell volt low L2 | sum volt high L1 | sum volt high L2 | sum volt low L1 | sum volt low L2 |
| 1 | chg temp high L1 | chg temp high L2 | chg temp low L1 | chg temp low L2 | dischg temp high L1 | dischg temp high L2 | dischg temp low L1 | dischg temp low L2 |
| 2 | chg overcurrent L1 | chg overcurrent L2 | dischg overcurrent L1 | dischg overcurrent L2 | SOC high L1 | SOC high L2 | SOC low L1 | SOC low L2 |
| 3 | diff volt L1 | diff volt L2 | diff temp L1 | diff temp L2 | reserved | reserved | reserved | reserved |
| 4 | chg MOS temp high | dischg MOS temp high | chg MOS temp sensor err | dischg MOS temp sensor err | chg MOS adhesion err | dischg MOS adhesion err | chg MOS open circuit err | dischg MOS open circuit err |
| 5 | AFE collect chip err | voltage collect dropped | cell temp sensor err | EEPROM err | RTC err | precharge failure | communication failure | internal communication failure |
| 6 | current module fault | sum voltage detect fault | short circuit protect fault | low volt forbidden chg fault | reserved | reserved | reserved | reserved |
| 7 | fault code (not a bitfield) | | | | | | | |

"L1" and "L2" are Daly's two alarm levels: L1 is a warning, L2 the protection
trip.

`core/daly_proto.c` still stores bytes 0-6 opaquely and reports only
`alarm_active`, and the pack page prints the raw hex. Decoding these into named
messages is the obvious next step now the table is known, but no bit has been
seen set on real hardware yet, so nothing is claimed by name.

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

- **Daly CAN Communications Protocol V1.0**, Dongguan Daly Electronics Co., Ltd,
  2019-06-11. Mirrored at
  <https://robu-prod-media.s3.ap-south-1.amazonaws.com/uploads/2022/02/Daly-CAN-Communications-Protocol-V1.0-1.pdf>
- <https://www.dalybms.com/news/daly-three-communication-protocols-explanation/>
- <https://github.com/Louisvdw/dbus-serialbattery/discussions/561>
- <https://diysolarforum.com/threads/decoding-the-daly-smartbms-protocol.21898/page-2>
- <https://diysolarforum.com/threads/daly-bms-can-and-arduino.22268/>

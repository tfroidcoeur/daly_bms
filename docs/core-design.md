# How `core/` is built

`core/` is the part of this project that decides what the numbers mean. It has
no screen, no CAN driver and no operating system. Everything on top of it — the
LVGL pages, the ESP-IDF app, the SDL simulator — is a consumer.

This document is the reasoning behind that shape. The file-header comments in
`core/` say what each module does; this says why the four of them are arranged
the way they are.

## Why it has no dependencies

Plain C99. No LVGL, no ESP-IDF, no `printf`, no allocation after init, no
threads, no blocking calls, no global mutable state. The same translation units
compile into `port/sim` and `port/esp32` unchanged.

That is not purity for its own sake. The protocol decoding and the poll policy
are the parts of this project most likely to be wrong and least likely to
announce it — a misread scaling factor shows a plausible number, not an error.
They are worth testing hard, and they are testable only while they are free of
the platform. `tests/` drives the whole stack with a fake clock and synthetic
frames, no hardware and no display.

The rule that keeps this true: **if a change to `core/` needs a platform header,
the change belongs somewhere else.**

## The four modules

```
    bms_model  ......  the data. Structs and derived aggregates, no policy.
        ^  ^
        |  |
   daly_proto  ......  bytes on the wire -> fields in the model.
        ^
        |
      poller  ........  who to ask, what to ask, when to give up.

    warnings  ........  reads the model, judges it. Depends on nothing else.
```

`bms_model` knows nothing about CAN. `daly_proto` knows nothing about
scheduling. `poller` is the only module that knows all three packs exist at
once. `warnings` reads a finished model and never writes to it.

`warnings` sits in `core/` rather than in `ui/` for the same reason the decoder
does: deciding that a 180 mV cell spread is worth shouting about is a judgement
that deserves a test, and it cannot have one if it is entangled with a label.

## Fixed-point everywhere

There are no floats in `core/`. Every value is an integer in a named unit, and
the unit is part of the field name:

| Suffix | Unit |
|---|---|
| `_mv` | millivolts |
| `_ma` | milliamps, positive = charging |
| `_c` | degrees Celsius |
| `_pct_x10` | tenths of a percent |
| `_mah` | milliamp-hours |

The reason is cross-platform identity. The host is x86-64 with a 387-descended
FPU; the ESP32-S3 is an Xtensa LX7. Integer arithmetic gives bit-identical
results on both, so a screenshot from the simulator is evidence about what the
firmware will show, not merely a good indication. Rounding happens once, at the
formatting boundary, in `ui_fmt_*()`.

Where a product could overflow, it is widened for one step and narrowed
immediately — see `bms_pack_watts()` in `core/bms_model.c:45`, which computes
microwatts in `int64_t` and scales down before returning.

## The poller is driven, not running

`poller` has no thread and no timer. Its entire interface is two calls:

```c
bool poller_tick(poller_t *p, uint32_t now_ms, can_frame_out_t *out);
bool poller_on_frame(poller_t *p, uint32_t id, const uint8_t *data,
                     uint8_t len, uint32_t now_ms);
```

`poller_tick` returns true when it wants a frame sent, and the caller sends it.
Time arrives as an argument. That single decision is what makes the polling
policy — ordering, spacing, timeouts, staleness — testable: a test advances
`now_ms` by hand and asserts on what comes out, with no sleeping and no bus.

### The schedule

Nine commands per pack, round-robin across three packs, one request every
`request_gap_ms` (40 ms). Fast commands run every round; slow ones every
`slow_divider` rounds (10th), because cell counts and cycle counts do not change
between seconds and the bus is shared.

| Step | Command | Cadence | Why |
|---|---|---|---|
| 1 | `0x94` status | slow | first: cell/temp counts gate 0x95 and 0x96 |
| 2 | `0x90` SoC | every round | pack V, A, SoC — the headline numbers |
| 3 | `0x91` cell min/max | every round | drives the spread warnings |
| 4 | `0x92` temp min/max | every round | |
| 5 | `0x93` MOS | every round | charge state, remaining capacity |
| 6 | `0x95` cell volts | every round | multi-frame |
| 7 | `0x96` cell temps | slow | multi-frame |
| 8 | `0x97` balance | slow | |
| 9 | `0x98` faults | slow | |

`0x94` is first in the list deliberately. Until it has answered, `cell_count`
and `temp_count` are zero, and steps 6 and 7 are skipped rather than guessed at
— there is no way to know a multi-frame set is complete without knowing how many
frames to expect.

A full round is nine commands times three packs at 40 ms, so about 1.08 s.

### Wrap safety

Both clocks in `core/` are `uint32_t` milliseconds, which wraps every 49.7 days.
Two comparisons have to survive that:

```c
} else if ((int32_t)(now_ms - p->next_tx_ms) < 0) {   /* core/poller.c:76 */
if ((uint32_t)(now_ms - p->last_seen_ms) > timeout_ms) {  /* bms_model.c:31 */
```

Both subtract first and test the difference. Neither compares two absolute
timestamps, which is the form that breaks at the wrap. This matters more than it
looks: a display meant to sit on a bulkhead for months should not lie once every
seven weeks.

## The multi-frame commit rule

This is the most important invariant in the decoder.

Commands `0x95` (cell voltages, 3 per frame) and `0x96` (temperatures, 7 per
frame) answer with a burst of frames, each tagged with its place in the burst.
They accumulate into `daly_rx_scratch_t` — a shadow copy plus a bitmap of which
positions have arrived — and are copied into the visible `cell_mv[]` / `temp_c[]`
only once every expected position is present.

```
frame 3 of 8 arrives  ->  written to scratch, bit 3 set, nothing published
frame 8 of 8 arrives  ->  bitmap complete, memcpy to the pack, valid = true
frame 5 never arrives ->  nothing published at all
```

A dropped frame therefore costs one poll round, not a half-written array. The
alternative — writing straight into `cell_mv[]` — would leave three cells
showing last round's values next to twenty-one showing this round's, and nothing
on screen would distinguish that from a real 40 mV step change.

The same reasoning drives `daly_apply_frame()` returning false on anything
malformed and leaving the pack untouched. **A corrupt frame must never poison
good data**, because the display has no way to render doubt.

### Each command owns its bitmap

`daly_rx_scratch_t` carries `seen_cells[]` and `seen_temps[]` separately, not one
shared bitmap. That is not tidiness. `0x95` and `0x96` are assembled at the same
time, and with a single bitmap an abandoned cell burst leaves marks that the
next temperature frame mistakes for its own:

```
0x95 frames 1..3 of 8 arrive, 4..8 are dropped  ->  cells_valid = 0   correct
0x96 frame 1 of 3 arrives                       ->  temps_valid = 1   WRONG
temp_c[] = 25 25 25 25 25 25 25 0 0 0 0 0 0 0 0 0
```

Nine sensors that never reported would publish as 0 C - a plausible reading, not
a visibly absent one - and the partial cell set would be wiped on the way past.
It takes a dropped frame to trigger, so it does not appear on the bench and does
appear on a noisy bus. `tests/test_core.c` covers both directions and the fully
interleaved case.

For the same reason `bms_model_age()` clears both bitmaps when a pack drops
offline: otherwise the burst that resumes after an outage can complete the one
the outage interrupted, publishing cells from both sides of the gap as a single
coherent reading.

### Where the frame numbering comes from

The two protocol sources disagree about byte 0. Daly's own document V1.0 says the
frame number starts at 0; the community layout this project was built from, and by
report most firmware in service, says 1. Nothing inside a frame declares which.

This is not a field where a wrong guess reads slightly off. Assume 1 against a
zero-based pack and the first frame of every burst is out of range and discarded,
so the bitmap never completes and **no cell data is ever published at all**.
Assume 0 and the same happens at the far end of the burst.

So `frame_position()` in `core/daly_proto.c` decides nothing and learns instead,
per pack and per command, from the two observations that only one scheme can
produce:

```
byte 0 == 0             only a 0-based burst sends this      -> base 0
byte 0 == frame count   only a 1-based burst gets this far    -> base 1
anything between        consistent with both                  -> discarded
```

Discarding while undecided is the whole point: a frame that cannot be placed must
not be stored anywhere. The cost is bounded and one-off — a 0-based pack settles
it on the first frame of its first burst and loses nothing, a 1-based pack settles
it on the last and shows its cells one round later. The base is latched for the
life of the pack, including across an offline period, because it is a property of
the firmware rather than of the burst; only the geometry changing resets the
bitmaps, and even then not the base.

`DALY_FRAME_BASE_UNKNOWN` is deliberately the zero value of its enum. Packs are
`memset` to zero in several places, and "not yet known" is the only default that
cannot publish data into the wrong slots.

## Liveness and correctness are separate

`poller_on_frame()` credits a pack as online for any well-formed response
addressed to us, *before* attempting to decode the payload
(`core/poller.c:132`):

```c
pack->online       = true;
pack->last_seen_ms = now_ms;
pack->frames_rx++;

daly_apply_frame(pack, cmd, data);   /* return value deliberately ignored */
```

A pack that is answering with payloads we reject is a different fault from a
pack that has gone silent — a wrong baud rate, a protocol variant, a duplicated
address. Conflating the two would show `NO DATA` for a BMS that is plainly
talking, and send you looking at the wiring instead of the decode table.

Staleness runs the other way and is entirely time-based: `bms_model_age()` is
called at the top of every `poller_tick`, and a pack unheard from for
`offline_timeout_ms` (5 s) drops offline regardless of what it last said.

### Online is not the same as reported

Because liveness is credited on the first reply to *any* command, an online pack
still has zeros in every field the schedule has not reached yet - and zero is a
legal SoC and a legal temperature. `soc_valid` (0x90) and `temp_minmax_valid`
(0x92) mark which of those fields hold a measurement rather than an initial
value, and `warnings_evaluate()` checks them before judging. Without that gate a
pack that has answered only 0x94 raises "SoC 0.0 % low" at alarm level and
"0 C too cold" - two fabricated lines that fill the whole footer and hide
whatever is real. Normally that window is one poll gap; for a BMS variant that
never answers 0x90 it is permanent.

## Where the judgement calls live

Every threshold is a `#define` at the top of `core/warnings.h:18`, with a
comment saying what it assumes. The defaults are for a 24S LiFePO4 bank, 60.0 V
empty to 87.6 V full. None of them is a law; they are collected in one place so
that changing chemistry or series count is a single edit rather than a search.

Two of them are worth calling out because they are not thresholds on a value but
on plausibility:

- `WARN_CELL_MIN_SANE_MV` / `WARN_CELL_MAX_SANE_MV` — a cell reading outside
  2.0–4.0 V is not a low cell, it is a broken measurement: a missing sense lead,
  a shorted cell, a dead channel. It is reported as a sense fault, not as a
  discharge warning.
- `WARN_PACK_MV_MISMATCH` — the packs are wired in parallel, so their voltages
  must track. A sustained 0.5 V gap is a bad joint, an open fuse, or a contactor
  that has not closed, and it is a bank-level alarm rather than a per-pack one.

The BMS's own `0x98` flags are the one rule whose severity is not ours to set.
Daly defines two levels per condition - level 1 a warning, level 2 the BMS
tripping its protection - and a warning at level 1 stays a warning here. Level 2,
a hardware fault, or a bit Daly marks reserved turning up set are alarms.

The "charging below freezing" rule decides *charging* from the BMS's own state in
`0x93`, never from the sign of the current. The sign is disputed between the
drivers in circulation (see below), and a rule keyed on it fails in the
dangerous direction - silent while charging - on a pack that runs the other way.

Warnings are sorted by severity, then pack, then code, so the first line is
always the one that matters most; the overview shows only the top few and a
`(+N more)` count.

The set is capped at `WARN_MAX` (12), and what survives the cap is chosen by the
same ordering - `push()` evicts the weakest line rather than refusing the
newcomer. That matters because the bank-level rules run last: dropping on
arrival would let three packs' worth of cell drift crowd out the one alarm that
says a fuse has blown.

## What is not verified

`docs/hardware/daly-can-protocol.md` now sets **Daly's own "CAN Communications
Protocol V1.0"** against the community reverse-engineering this module was built
from. They agree byte for byte on `0x90`, `0x91`, `0x92`, `0x93` and `0x97`,
which is good evidence and not the same as having seen it work. This section says
it again because `core/` is where a wrong offset turns into a confident number on
a screen.

Where the two sources disagree, the code refuses to choose:

| | Disagreement | How it is handled |
|---|---|---|
| `0x95` / `0x96` byte 0 | numbered from 0 or from 1 | learned from the traffic, above |
| `0x94` bytes 5-6 | reserved, or a cycle count | read, with `cycles_valid` saying whether anything was there |
| `0x90` current direction | not stated; the drivers disagree | `DALY_CURRENT_SIGN`, a build setting settled at bring-up |

On the first two, every driver written against real hardware sides with the
community layout - 1-based frames, cycles present - so that is what to expect.

`cycles_valid` exists so the panel can stay silent rather than print a zero that
reads as a brand new battery. It is also the cheapest possible instrument for the
question: whether the pack page's `CAPACITY` field shows a cycle count *is* the
answer for that firmware.

Nothing here has been run against a real BMS. Before trusting any value on the
panel, build the firmware with `CONFIG_BMS_RAW_LOGGER` set — see
`port/esp32/main/main.c:46` — which dumps every transmitted and received frame
over USB serial and draws nothing. Compare that against Daly's own app, one pack
at a time, and correct the table before correcting anything else.

The two scalings most likely to be wrong, because both are biased rather than
plain, are in `core/daly_proto.h:117`:

```c
current_ma = (raw - 30000) * 100 * DALY_CURRENT_SIGN  /* 0.1 A, 30000 offset */
temp_c     = raw - 40                                 /* 40 degree offset */
```

If the current reads 3000 A at rest, that offset is the first thing to check. If
it reads the right size but positive under load, that is the sign - set
`CONFIG_BMS_INVERT_CURRENT` - and nothing else is wrong.

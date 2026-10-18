# Remote reporting protocol

How the display reports the bank to the club website (`avia-bms` WordPress
plugin on <https://www.avia-dpc.be>). This document is the contract between the
firmware (`core/remote.c`, `core/snapshot_json.c`) and the plugin; both test
suites check against the files in [`remote/`](remote/).

## Shape

The device always initiates. The server never contacts the device: it sits
behind the club's NAT and is in deep sleep most of the time.

```
device                                   server
  |-- GET  /device/checkin?... ----------->|   every check-in interval
  |<-- {"cmd":"none","next_s":900} --------|
  ...
  |-- GET  /device/checkin?... ----------->|   a refresh or the schedule is due
  |<-- {"cmd":"report","next_s":900,"req":"…"} |
  |   wake packs, collect (≤ 60 s)         |
  |-- POST /device/report {snapshot} ----->|
  |<-- 204 --------------------------------|
  ...
  |   screen on (touch, dashboard)         |
  |-- POST /device/live?every=5 {snapshot} |   every live interval, unasked
  |<-- 204 --------------------------------|
```

Base URL: `https://www.avia-dpc.be/wp-json/avia-bms/v1`. The device uses the
exact `www` host, follows **no** redirects (a 3xx is an error), and sends:

```
Authorization: Bearer <token>
```

`<token>` is 64 lowercase hex characters (32 random bytes). The server stores
only `sha256(token)` and compares with `hash_equals`.

## `GET /device/checkin`

Query parameters, all optional, unknown ones ignored:

| Name      | Format                         | Meaning                                    |
|-----------|--------------------------------|--------------------------------------------|
| `fw`      | `^[0-9]{1,3}(\.[0-9]{1,3}){2}$` | firmware version                           |
| `state`   | `local` \| `standby`           | someone is using the screen, or not        |
| `wake`    | `boot` \| `timer` \| `touch` \| `can` | what woke the device for this check-in |
| `vbat_mv` | `^[0-9]{1,5}$`                 | board battery voltage, when an 18650 is used |

Reply `200`, `Content-Type: application/json`, at most 512 bytes:

```json
{"cmd":"none","next_s":900}
{"cmd":"report","next_s":900,"req":"3f9a0c27d1e84b56"}
{"cmd":"none","next_s":900,"live_s":10}
```

| Field    | Rule                                                                  |
|----------|-----------------------------------------------------------------------|
| `cmd`    | `"none"` or `"report"`. Anything else: the device treats the whole reply as invalid. |
| `next_s` | integer, `0` = timed check-ins disabled, else `1` … `1209600` (14 days). Out of range: reply invalid. |
| `req`    | present iff `cmd` is `"report"`; `^[0-9a-f]{16}$`. Otherwise: reply invalid. |
| `live_s` | optional integer `0` … `3600`: seconds between live reports while the screen is on, `0` = off. Absent: the device uses its own setting (console `live`, default 5). Out of range: reply invalid. |

An invalid reply, a non-200 status, a transport error or a timeout counts as a
failed check-in (back-off, below). Other status codes the server uses: `401`
bad or missing token, `429` rate-limited.

## `POST /device/report`

Body: one snapshot (below), `Content-Type: application/json`, at most 16 KiB.
The `req` inside must be the server's **currently outstanding** request id
(single use, expires 15 min after it was handed out).

| Status | Meaning                                        |
|--------|------------------------------------------------|
| `204`  | stored                                         |
| `400`  | body is not a valid snapshot                   |
| `401`  | bad or missing token                           |
| `409`  | `req` is not the outstanding request (stale, reused, unknown) |
| `413`  | body over 16 KiB                               |

The device does not retry a report; the server re-issues the request at a later
check-in if it still wants one.

## `POST /device/live?every=<s>`

While the screen is on (LOCAL: a touch, a dashboard request, or current
flowing in the bank keeps it so), the device posts a snapshot every live
interval without being asked. Same body as a report, but `"req": null`.
`every` is the device's interval in seconds (`1` … `3600`); the page uses it
to tell whether live data is still coming in (at most three intervals, and at
least 15 s, old). A live snapshot replaces the stored one like a report does.

| Status | Meaning                                        |
|--------|------------------------------------------------|
| `204`  | stored                                         |
| `400`  | not a valid snapshot, `req` not null, or `every` missing or out of range |
| `401`  | bad or missing token                           |
| `409`  | live reports are switched off in WP admin      |
| `413`  | body over 16 KiB                               |
| `429`  | more than 30 in a minute                       |

The first live snapshot of a screen session waits until every pack has given
its full data, or 15 s. Failures (out of Wi-Fi range while winching is normal)
are not retried: the next one comes at the usual interval. Live reports do not
count against the report cap.

Security note: unlike a report, a live snapshot is not tied to a request, so a
stolen token can overwrite the page's snapshot at will (30 a minute). It was
already able to answer every real request falsely; rotating the token in WP
admin ends both, and `live_s` = 0 refuses live snapshots altogether.

## Snapshot

Compact JSON, keys in exactly this order (the firmware's golden tests compare
byte for byte). All numbers are integers in the units the key names: `_mv`
millivolts, `_ma` milliamps, `_w` watts, `_x10` tenths, `_c` °C, `_mah` mAh.
`null` means "not reported by the BMS yet", never zero.

```json
{
  "schema": 1,
  "req": "3f9a0c27d1e84b56",            (null in a live snapshot)
  "fw": "1.1.0",
  "uptime_s": 41,
  "bank": {
    "online": 3, "mv": 83250, "ma": -12000, "w": -999,
    "soc_x10": 765, "state": "discharging", "charger": false,
    "cell_delta_mv": 25
  },
  "packs": [
    {
      "addr": 1, "online": true,
      "soc_x10": 760, "mv": 83200, "ma": -4000, "w": -332,
      "state": "discharging", "chg_mos": true, "dsg_mos": true,
      "cycles": 12, "remaining_mah": 152000,
      "cell_min_mv": 3460, "cell_max_mv": 3480,
      "temp_min_c": 18, "temp_max_c": 19,
      "cells_mv": [3470, ...], "temps_c": [18, 19],
      "balancing": [],
      "faults": [{"pos": 2, "trip": true, "text": "cell voltage low, tripped"}]
    }
  ],
  "warnings": [
    {"code": "cell_spread", "level": "warn", "pack": 2, "text": "PACK 2  cell spread 120 mV"}
  ]
}
```

Rules:

- `packs` always has exactly 3 entries, addresses 1, 2, 3 in order.
- An offline pack is `{"addr":N,"online":false}` and nothing else.
- For an online pack every other key is present; a value not yet reported is
  `null` (`soc_x10`, `mv`, `ma`, `w` together on `soc_valid`; `temp_min_c`,
  `temp_max_c` on `temp_minmax_valid`; `cycles` on `cycles_valid`; `cells_mv`
  on `cells_valid`; `temps_c` on `temps_valid`). `cell_min_mv`/`cell_max_mv`
  are `null` while both are 0.
- `cells_mv`: `cell_count` entries (1 … 48). `temps_c`: `temp_count` entries
  (1 … 16), each −40 … 125.
- `balancing`: 1-based cell numbers currently balancing, ascending.
- `faults`: every set 0x98 bit, in wire order (`pos` = byte × 8 + bit, 0 … 55),
  `trip` per `daly_fault_is_trip()`, `text` per `daly_fault_describe()`.
- `state`: `"idle"` \| `"charging"` \| `"discharging"`.
- `bank`: from `bms_model_summary()`. With no pack online, `mv`, `ma`, `w`,
  `soc_x10` are `null` and `online` is 0.
- `warnings`: `warnings_evaluate()` order, at most 12; `code` is the
  `warn_code_t` name in snake case without the `WARN_` prefix (`pack_offline`,
  `pack_voltage_mismatch`, `cell_count_mismatch`, `cell_implausible`,
  `cell_spread`, `bms_fault`, `soc_low`, `temp_high`, `temp_low`,
  `charging_below_zero`); `level` is `"warn"` \| `"alarm"`; `pack` is 0 for the
  bank, else 1 … 3; `text` is `warnings_format()`.
- Every string the device sends (`text` fields, `fw`, `req`) matches
  `^[A-Za-z0-9 .,:%/()+-]{0,64}$`. The encoder replaces any other character
  with a space and truncates at 64, so nothing ever needs escaping.

The schema is [`remote/snapshot.schema.json`](remote/snapshot.schema.json);
[`remote/fixtures/`](remote/fixtures/) holds golden snapshots (one line, no
trailing newline) produced from the models described in
[`remote/fixtures/README.md`](remote/fixtures/README.md).

## Device-side rules

- **Back-off:** after a failed check-in, retry after
  `min(interval, 15 min) × 2^(failures−1)`, capped at 60 min, plus up to 10 %
  random jitter; the first success resets it. With timed check-ins disabled, a
  failure is not retried until the next local wake.
- **Report cap:** at most 24 reports in any rolling 24 h; a `report` beyond
  that is treated as `none`.
- **Interval floor:** a console setting `min_checkin` (seconds, default 1)
  raises any shorter `next_s` to itself.
- **Local wake:** a touch or a request to the virtual BMS triggers a check-in
  right away, even with timed check-ins disabled - this is how a re-enabled
  interval reaches a device that had stopped checking in.
- **Deep or awake:** an interval of 60 s or more is slept in deep sleep; a
  shorter one is waited out awake, Wi-Fi associated, CAN quiet.

## Server-side rules (summary; the plugin is authoritative)

- One outstanding request at a time. A manual refresh is accepted only from a
  signed-in, allowlisted user, at least 10 min after the previous one, at most
  12 a day.
- The automatic report is off, or every 1 h … 7 days anchored at a time of
  day; it is due when a check-in arrives at or after that time.
- `next_s` comes from the admin setting (default 900).

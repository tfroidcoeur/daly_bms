# Golden snapshots

Each `*.json` here is the exact output of `snapshot_json_encode()` (one line,
no trailing newline) for the model described below. `tests/test_snapshot.c`
builds these models and compares byte for byte; the `avia-bms` plugin's PHP
tests feed the same files to its validator, which must accept every one.

They are a golden master: produced by the encoder once
(`./build/test_snapshot --write-fixtures docs/remote/fixtures`), then checked by
hand against [`../../remote-protocol.md`](../../remote-protocol.md) and against
[`../snapshot.schema.json`](../snapshot.schema.json) before being committed. A
change to any of them is a protocol change and needs the plugin's copy updated
too.

All snapshots use `req = "3f9a0c27d1e84b56"`, `fw = "1.1.0"`, `uptime_s = 41`.

## Base model ("normal")

Every pack `p` = 1, 2, 3 (Daly address `p`) online, with every validity flag set:

| Field | Value |
|---|---|
| `pack_mv` | 83150 + 50·p (83200, 83250, 83300) |
| `pack_ma` | −4000 (discharging) |
| `soc_pct_x10` | 755 + 5·p (760, 765, 770) |
| `charge_state` | 2 (discharging); `chg_mos`, `dsg_mos` true |
| `charger_present`, `load_present` | false, true |
| `cycles` | 11 + p (cycles_valid) |
| `remaining_mah` | 152000 |
| `cell_count`, cells | 24; cell i (1-based) = 3465 + (i mod 5) mV |
| `cell_min_mv`/`idx`, `cell_max_mv`/`idx` | 3465 / 5, 3469 / 4 |
| `temp_count`, temps | 2; 18, 19 °C |
| `temp_min_c`/`idx`, `temp_max_c`/`idx` | 18 / 1, 19 / 2 |
| `balance_bits` | 0 |
| `alarms` | all zero |

## Variants

- **`normal.json`**: the base model.
- **`weak-cell.json`**: base, plus on pack 2: cell 7 = 3345 mV,
  `cell_min_mv` = 3345 / idx 7; `balance_bits` = cells 1 and 4 (bits 0, 3);
  `alarms[0]` bit 0 set (`alarm_active`).
- **`offline.json`**: base, plus pack 3 offline (`online` false), and on pack 1
  `cycles_valid` and `temps_valid` false.
- **`all-offline.json`**: base with all three packs offline.

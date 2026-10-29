# Changelog

All notable changes to this project. Versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- The virtual BMS speaks Victron's CAN-bus BMS protocol instead of Daly's, for
  the winch controller: the bank goes out as `0x355` (SoC, SoH) and `0x356`
  (voltage, current, warmest temperature), 11-bit, once a second while the
  display polls the packs. The Daly emulation at 0x10 is gone, and with it the
  wake on a dashboard request; `CONFIG_BMS_VIRTUAL_ADDR` is replaced by
  `CONFIG_BMS_VICTRON` (on by default), `sim_headless --virtual ADDR` by
  `--victron`, and `daly_sim.py`'s dashboard poller by a `--victron` listener.

- The two capacitive touch pads are gone: too unreliable behind the enclosure
  wall in the winch. One mechanical push button between GPIO1 (P1 pin 7) and
  GND replaces them and works like the KEY: a short press turns the page, a
  hold over 800 ms opens the cell detail, and a press wakes the board from
  deep sleep (ext1, together with CAN RX) and opens the console within 10 s
  of a reset. The press that wakes the board does not also turn a page.
- The touch-monitor build mode is removed.

## [2.0.0] - 2026-10-18

Remote reporting to the club website, and a display that is no longer always
on. A major version because the default behaviour changes: the board stops
polling and deep-sleeps when nobody uses it, the KEY button is given up for
the transceiver's Rs pin, and the partition table changes (flash the whole
image, not just the app). Tested on the bench against the live site: Wi-Fi,
HTTPS check-in, deep sleep and touch wake.

### Remote reporting

- Reports the bank to the club website (`avia-bms` plugin on avia-dpc.be):
  the board checks in over HTTPS, and uploads a JSON snapshot when the site
  asks - a manual refresh or the automatic schedule. Protocol:
  [docs/remote-protocol.md](docs/remote-protocol.md).
- The bus is quiet unless someone needs data, so the Daly packs can reach
  their own sleep: the screen shows "Zzz" and the board deep-sleeps between
  check-ins. A touch or a dashboard request to the virtual BMS wakes it and
  the screen for 5 minutes.
- While the screen is on, a live snapshot every 5 s (`live <s>` on the
  console, or WP admin; 0 = off) to `POST /device/live`; the page updates
  itself and shows "Live". Out of Wi-Fi range it simply keeps trying.
- The screen stays on while any pack reports charging or discharging.
- Wi-Fi icon, top right: filled signal bars by strength when connected,
  hollow when set up but not connected, none when there is no Wi-Fi.
- Wi-Fi, server and device token are set over the USB console (touch a pad
  within 10 s of a reset) and kept in NVS; secrets are never printed. A failed
  connect logs the access point's reason and the networks in range.
- GPIO18 can drive the transceiver's Rs pin (standby in deep sleep); the KEY
  button is then no longer read. `CONFIG_BMS_CAN_RS_GPIO`, -1 = not wired.
- Partition table: 1.5 MB single app.
- Host: `sim_headless --server` runs the same code against a website,
  `daly_sim.py --scenario sleepy` has packs that fall asleep (`idle`: a bank
  at rest), and
  `tools/e2e/run.sh` tests the whole loop against a real WordPress.
- Bench checklist: [docs/bring-up-remote.md](docs/bring-up-remote.md).

### Fixed during bring-up

- Wi-Fi was never put in station mode, so every connect failed
  (`ESP_ERR_WIFI_MODE`).
- The network watchdog restarted the board right after a console `checkin`:
  it compared against a clock read before the job was posted.

### Documentation

- The Rs rework through the KEY button's pad (lift the 10 kOhm's GND end),
  resetting the board without a reset button, quitting `idf.py monitor`, and
  the `bms>` console commands, in the README and the bench checklist.

## [1.0.0] - 2026-10-03

First release. Tested on the reference setup: three 24S LiFePO4 packs with
Daly BMS units in parallel, a Waveshare ESP32-S3-RLCD-4.2 in a PLA enclosure,
and an SN65HVD230 transceiver.

### Monitoring

- Polls three Daly BMS units (addresses 0x01-0x03) round-robin over one
  250 kbit/s CAN bus, using commands 0x90-0x98.
- Overview page: bank SoC, a 0-350 A magnitude-only current dial with power,
  and a voltage column. Pack pages show each BMS in full; the cells page shows
  all 24 cells with min/max marked.
- A pack that stops answering shows `NO DATA` and drops out of the bank totals.
- Warnings, worst first: pack offline, parallel voltage mismatch, cell count
  mismatch, implausible cells, cell spread, low SoC, temperature, charging
  below freezing, and the BMS's own fault flags by name at Daly's severity.

### Protocol

- Learns per pack whether multi-frame bursts are numbered from 0 or 1; real
  Daly firmware numbers from 1.
- Multi-frame cell and temperature sets are published only when complete.
- Cycle count shown only when the pack actually reports one.
- "Charging" comes from the BMS's own state, not the sign of the current.
- Current direction is a build setting (`CONFIG_BMS_INVERT_CURRENT`).

### Virtual BMS

- Answers as a fourth Daly BMS at `CONFIG_BMS_VIRTUAL_ADDR` (default 0x10,
  0 = off), presenting the three packs as one for dashboards that read a
  single BMS: voltage and SoC averaged, current and capacity summed, extremes
  across packs, faults combined.

### Controls

- Two capacitive pads behind the front panel, tuned for a PLA wall, with a
  reset for a pad whose benchmark has been frozen for 15 s. The onboard KEY
  button works too (short press / 800 ms hold).

### Tooling

- Host simulator (SDL) and headless monitor running the same code against
  SocketCAN, with a three-pack Daly simulator and scripted screenshots.
- Diagnostic firmware modes: raw CAN frame logger, CAN transceiver loop test,
  and a touch pad monitor.
- Host unit tests over the protocol, model, poller, warnings and virtual BMS.

[2.0.0]: https://github.com/tfroidcoeur/daly_bms/releases/tag/v2.0.0
[1.0.0]: https://github.com/tfroidcoeur/daly_bms/releases/tag/v1.0.0

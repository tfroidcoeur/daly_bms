# Changelog

All notable changes to this project. Versions follow
[Semantic Versioning](https://semver.org/).

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

[1.0.0]: https://github.com/tfroidcoeur/daly_bms/releases/tag/v1.0.0

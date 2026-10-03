# daly_bms_monitor

A standalone display for three Daly BMS units in parallel on one battery
bank. It polls them over a shared CAN bus and shows state of charge, current
and per-cell health on a small LCD.

The protocol, model and UI are portable C99 and LVGL 9, so any board running
FreeRTOS (or similar) with an LCD and a CAN controller can host it; see
[docs/porting.md](docs/porting.md). A host simulator runs the same code on
Linux against SocketCAN.

*Written with [Claude](https://claude.ai), Anthropic's AI assistant.*

## Reference setup

The code is developed and tested on one concrete setup, used as the example
throughout the docs: three **24S LiFePO4 packs, each with a Daly smart BMS**
(addresses 0x01-0x03), wired in parallel. The display is a
**Waveshare ESP32-S3-RLCD-4.2** (ESP32-S3, 4.2" 400 x 300 reflective monochrome
LCD) in a 3D-printed PLA enclosure, with two copper-tape touch pads behind the
front panel. It talks to the packs through an **SN65HVD230** transceiver on the
S3's built-in TWAI controller, at 250 kbit/s. On the bench, an
**IXXAT USB-to-CAN V2** connects the laptop to the same bus. Specs, GPIO map and
wiring are in [`docs/hardware/`](docs/hardware/); the short version is in
[wiring.md](docs/hardware/wiring.md#in-short).

## What it shows

| Page         | Contents                                                                                       |
|--------------|------------------------------------------------------------------------------------------------|
| **Overview** | Bank SoC column, 0-350 A current dial with power, voltage column, two-line status footer       |
| **Pack 1-3** | One BMS: SoC, V/A/W, cell min/max and spread, temperatures, MOSFETs, cycles, state             |
| **Cells**    | All 24 cells as bars on the 2.50-3.65 V range, min/max arrowed                                 |

![Overview, pack detail and cell pages](docs/img/ui-pages.png)

*Simulator output at the panel's resolution and 1 bpp: the overview, the same
with two alarms, and the cells of a drifted pack.*

- The current dial shows **magnitude only**, 0 to `UI_GAUGE_MAX_A`. Direction
  comes from the `CHARGING` legend and the status line.
- Warnings replace the footer lines, worst first: bold with a warning glyph,
  alarms inverted.
- A pack that stops answering shows `NO DATA` and drops out of the bank totals.
- Warning rules are in `core/warnings.c` and unit-tested. Thresholds are
  `#define`s in `core/warnings.h`, set for the reference 24S LiFePO4 bank
  (60.0-87.6 V, also the voltage column's range, `UI_VBAR_CELLS` in `ui/ui.h`).

**Controls:** two inputs, one cycling pages and one opening the cell detail. On
the reference board these are the touch pads, or the onboard KEY button with a
short press and an 800 ms hold.

## Virtual BMS

The display also answers on the bus as a
**fourth Daly BMS that is the whole bank**, so a dashboard that reads only one
Daly BMS can read all three packs. The dashboard polls the virtual address
(`CONFIG_BMS_VIRTUAL_ADDR`, default `0x10`, `0` = off) with normal `0x90`-`0x98`
requests.

| Field                                           | Combined as                                                         |
|-------------------------------------------------|---------------------------------------------------------------------|
| Voltage, SoC                                    | mean                                                                |
| Current, remaining capacity                     | sum                                                                 |
| Cell / temperature min and max (`0x91`, `0x92`) | extremes across all packs                                           |
| Cell voltages (`0x95`)                          | per position, the pack whose cell strays furthest from the bank mean |
| Temperatures (`0x96`)                           | all packs' sensors in sequence, up to 16                            |
| Charge / discharge MOSFET                       | on only if on in every pack                                         |
| Faults (`0x98`)                                 | OR across packs, plus *communication failure* while a pack is offline |
| Cycles                                          | highest                                                             |

Cell voltages are not averaged: the packs are separate series strings, and a
mean would hide a weak cell. Only online packs count; unreported data goes
unanswered rather than zero, and with every pack offline the virtual BMS is
silent. Current uses the real packs' sign convention. Rules are in
`core/virtual_bms.c`.

## Layout

```
core/        portable C99: Daly decoding, data model, poll scheduler, warnings,
             virtual BMS. No platform headers, no I/O, no threads.
ui/          LVGL 9 screens, shared by simulator and firmware
port/sim/    host: SocketCAN + SDL
port/esp32/  reference port: ESP-IDF app - TWAI, ST7305 panel, touch pads
tools/       daly_sim.py (fake three-BMS bus), vcan and adapter setup
tests/       host unit tests over core/
docs/        core design, porting guide, fonts, reference hardware
```

## Documentation

| Document | Contents |
|---|---|
| [core-design.md](docs/core-design.md) | How `core/` is built and why |
| [porting.md](docs/porting.md) | What a port to other hardware supplies |
| [fonts.md](docs/fonts.md) | The X11 misc-fixed bitmap fonts and how to regenerate them |
| [hardware/](docs/hardware/) | The reference hardware: board, transceiver, wiring |
| [daly-can-protocol.md](docs/hardware/daly-can-protocol.md) | Daly CAN frames, and what real packs were seen to send |
| [bring-up-usb-can.md](docs/hardware/bring-up-usb-can.md) | First contact with the packs from a laptop |
| [bring-up-board.md](docs/hardware/bring-up-board.md) | First flash of the board, touch pads, CAN |

## Building and running on the laptop

Everything except the board drivers (panel, touch, CAN controller) runs on the
host against a virtual CAN bus.

```bash
sudo apt install build-essential cmake ninja-build libsdl2-dev can-utils

python3 -m venv .venv
.venv/bin/pip install -r tools/requirements.txt

./tools/setup-vcan.sh              # creates and brings up vcan0

cmake -S . -B build -G Ninja       # fetches LVGL 9.5.0 on first configure
cmake --build build
ctest --test-dir build --output-on-failure
```

If your packs report discharge above Daly's 30000 current bias, configure with
`-DBMS_INVERT_CURRENT=ON` (see `DALY_CURRENT_SIGN` in `core/daly_proto.h`).

Then, in two terminals:

```bash
.venv/bin/python tools/daly_sim.py --scenario weak-cell
./build/sim vcan0
```

Simulator keys: `n` next page, `N` cell detail, `q` quit. It renders at 1 bpp
like the panel; `SIM_REFLECTIVE_TINT` in `port/sim/sdl_display.c` previews the
panel's grey-green background.

### Other host tools

```bash
./build/sim_headless vcan0                  # same core, as text
./build/sim_headless vcan0 --virtual 0x10   # ...also answering as the virtual BMS
./build/test_core                           # unit tests
candump vcan0                               # raw bus
```

### Simulator scenarios

| Scenario    | Exercises                                                     |
|-------------|---------------------------------------------------------------|
| `normal`    | three healthy packs discharging                               |
| `weak-cell` | pack 2 has a cell drifting 120 mV low                         |
| `offline`   | pack 3 silent for 20 s: `NO DATA` and bank totals             |
| `charging`  | current reversed, SoC rising                                  |
| `alarm`     | pack 1 raises an alarm flag                                   |
| `faults`    | enough at once to overflow the footer (`+N more`)             |
| `winch`     | pulls ramping the bank to ~600 A, past the gauge's range      |

`--drop 0.15` discards random replies to exercise multi-frame reassembly.
`daly_sim.py` uses python-can, so it can also drive a real adapter to
bench-test the board:

```bash
.venv/bin/python tools/daly_sim.py -I slcan -i /dev/ttyACM0 -b 250000
```

### Unattended screenshots

```bash
SIM_SCRIPT="3000:n,1500:n,1500:N" SIM_SHOT_DIR=shots SIM_QUIT_MS=12000 \
  ./build/sim vcan0
```

Saves a PPM just before each scripted key press and one at quit.
`tools/make-screenshots.sh` regenerates the images in `docs/img/` this way.

### Real CAN hardware

Both host binaries take the interface name, so a USB-CAN adapter replaces
`vcan0`:

```bash
./build/sim_headless can0 --raw    # every frame with its decode, no filter
./build/sim_headless can0          # decoded model
./build/sim can0                   # the screens
```

Adapters supported in mainline:

```bash
# gs_usb / candleLight / PCAN
sudo ip link set can0 type can bitrate 250000 && sudo ip link set up can0

# slcan (CANable, USBtin) - -s5 is 250 kbit/s
sudo slcand -o -c -s5 /dev/ttyACM0 can0 && sudo ip link set up can0
```

The IXXAT USB-to-CAN V2 needs an out-of-tree driver:
`sudo ./tools/setup-ixxat.sh`.
When the bus looks dead, check `ip -details -statistics link show can0` for
error counters. Both, and the full bench procedure, are in
[bring-up-usb-can.md](docs/hardware/bring-up-usb-can.md).

## Building for the reference board

This section is specific to the ESP32-S3-RLCD-4.2; for other hardware see
[docs/porting.md](docs/porting.md). It needs ESP-IDF **5.5** (built and
tested on 5.5.5): 6.x replaced the `driver/twai.h` API this uses, and the touch
pads use the `esp_driver_touch_sens` driver.

```bash
sudo apt install -y flex bison
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --depth 1 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf && ./install.sh esp32s3
. ~/esp/esp-idf/export.sh                # in every new shell
```

```bash
idf.py -C port/esp32 set-target esp32s3
idf.py -C port/esp32 build
idf.py -C port/esp32 flash monitor
```

Use `-C`, not `cd`: the repository root is the host CMake project. Options
(virtual BMS address, current direction, diagnostic modes) are under *Daly BMS
monitor* in `idf.py -C port/esp32 menuconfig`.

### Flashing

One USB-C cable carries flashing and the console (USB serial/JTAG), as
`/dev/ttyACM0`.

| Problem                            | Fix                                                                                     |
|------------------------------------|-----------------------------------------------------------------------------------------|
| Nothing at `/dev/ttyACM*`          | Download mode: hold **BOOT**, click **PWR**, release BOOT. PWR is a latch (long press off) |
| `Permission denied` on the port    | `sudo usermod -aG dialout $USER`, then log in again                                     |
| Flash succeeds, board does not run | `idf.py -C port/esp32 erase-flash`, then flash again                                    |
| Wrong port picked                  | add `-p /dev/ttyACM0`                                                                   |

[bring-up-board.md](docs/hardware/bring-up-board.md) walks through the first
flash.

### Display driver

`port/esp32/main/st7305.c` drives the ST7305, which packs a 4-wide x 2-tall
pixel block per byte (15000 bytes for 300 x 400). The glass scans 300 x 400
portrait, so `flush_cb` rotates the 400 x 300 UI (`BOARD_ROTATE_CW` in
`port/esp32/main/board.c`). LVGL (9.5.0 on both sides)
renders RGB565 and both ports reduce it to one bit with the same
`ui_px_is_paper()`, so the simulator shows exactly what the glass will.

### Threading

Two FreeRTOS tasks: `app_main` on core 0 polls CAN and handles input, and LVGL
renders on core 1 at higher priority, behind a recursive lock. The rules are
in [docs/porting.md](docs/porting.md#5-tasks-and-a-lock-on-an-rtos).

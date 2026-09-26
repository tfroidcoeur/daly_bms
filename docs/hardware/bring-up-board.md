# Board bring-up: first flash of the ESP32-S3-RLCD-4.2

The build steps live in the [README](../../README.md#building-for-the-board).
This is what to do *after* the first `idf.py flash` - specifically, how to work
out what is wrong when the panel does not light up, because **the display driver
has never run on hardware**.

Everything else has been exercised: the decoder, the model, the poller and the
warnings are covered by 711 host checks, and the UI has been walked page by page
in the simulator. `st7305.c` and the rotation in `board.c` are the parts with no
evidence behind them at all.

So bring the screen up **before** connecting anything to CAN. One unknown at a
time.

---

## 0. Handle the board carefully

From Waveshare: the screen is a fragile precision component. Do not use it as a
stress point when plugging in USB-C or fitting an 18650. Cracking and display
faults from rough handling are not covered by warranty.

There is **no RESET button** - BOOT, KEY and PWR only. PWR is a hardware power
latch: click for on, long press for off.

## 1. Check the toolchain before the board

```bash
. ~/esp/esp-idf/export.sh
idf.py --version                  # must be 5.x
```

**Not 6.x.** v6.0 replaced `driver/twai.h` with a node-based API
(`esp_twai_onchip.h`) and `twai_link.c` is written against 5.x. It will not
compile, and the error will not obviously say why.

## 2. Plug in and confirm the chip talks

USB-C to the board. The console is the built-in USB serial/JTAG, so one cable
does flashing and logging both.

```bash
ls /dev/ttyACM*                   # expect ttyACM0
```

If nothing appears, hold **BOOT**, click **PWR**, release BOOT - that forces
download mode. With no reset button this is the only way back from a firmware
that wedges the USB stack.

## 3. Flash and read the boot log

```bash
idf.py -C port/esp32 flash monitor
```

Read the log before looking at the screen. These four lines are the checkpoints,
in order:

```
I (xxx) st7305: up: 300x400, 15000 byte framebuffer, 10000000 Hz
I (xxx) board:  LVGL up at 400x300 landscape -> 300x400 panel, 1 bpp
I (xxx) board:  LVGL task running on core 1
I (xxx) twai:   up at 250 kbit/s on TX=1 RX=2
```

| Stops at | Meaning |
|---|---|
| nothing at all | Not running. Check download mode, then `idf.py -C port/esp32 erase-flash` and reflash |
| `panel init failed` | SPI or the init sequence - see step 4 |
| `no memory for the LVGL framebuffer` | PSRAM not detected. Confirm `CONFIG_SPIRAM_MODE_OCT=y` survived; this is an N16R8 module |
| `LVGL task did not start` | Out of internal RAM - unexpected, report it |

`Guru Meditation` at any point is a real bug, not a wiring problem. Keep the
backtrace: `idf.py -C port/esp32 monitor` decodes it to source lines
automatically.

## 4. What the screen is doing

This is the part with no prior evidence. Match the symptom:

| What you see | Most likely cause | What to try |
|---|---|---|
| Completely blank, log clean | Panel never left sleep, or no VCOM | The init sequence ran but produced nothing - scope CS/CLK/MOSI if you can, or suspect `0x11` SLPOUT timing |
| All black, or all noise | `CMD_COLMOD`/`CMD_DUTY` wrong for this panel revision | Compare `run_init_sequence()` against Waveshare's `display_bsp.cpp` again - it matched byte for byte, but the panel revision may not |
| Recognisable image, upside down | Rotation guess was wrong | Flip `BOARD_ROTATE_CW` to `0` in `port/esp32/main/board.c:40` |
| Recognisable image, mirrored | `MADCTL` (`0x36`, currently `0x48`) | Try `0x08` or `0xC8` |
| Image shifted sideways by a few pixels | Column window offset | `WIN_COL_START` in `st7305.c` is `0x12`; each step is 12 pixels |
| Scrambled into diagonal bands | Block packing | `st7305_byte_index()` / `st7305_bit_mask()` in `st7305.h` - but these match both Waveshare and Zephyr |
| Correct but very faint | Low power mode | `st7305_set_low_power(dev, false)` - the init already sends HPM |

**Rotation is the expected failure.** The comment at `board.c:40` says as much:
the panel scans as 300x400 portrait, the UI is 400x300 landscape, and which
quarter turn is right depends on how the board ends up mounted. There was no
board when that was written. If the picture is legible but wrong way up, change
one `#define` and reflash - that is a success, not a fault.

## 5. Expect CAN errors, and read them as good news

With no transceiver wired, nothing ACKs our frames. The controller accumulates
errors, reaches bus-off, and the health check recovers it - every few seconds:

```
W (xxx) twai: bus-off; initiating recovery
I (xxx) twai: bus recovered; running again
```

That loop is the recovery path working correctly. It is noisy while you are
debugging the panel; ignore it until step 7.

## 6. The controls

One KEY press should cycle Overview -> Pack 1 -> Pack 2 -> Pack 3 -> Overview,
and a hold over 800 ms should open the cell detail for the current pack. Every
pack will read `NO DATA` - nothing is on the bus yet. That is the correct
display, and it confirms the whole chain: input, LVGL lock, page switch, repaint.

The two capacitive pads do the same two things, one each, and need no hold. If
they do nothing, build the **touch monitor** mode (menuconfig -> Daly BMS
monitor -> Build mode) rather than guessing: it prints each pad's benchmark,
smoothed reading and margin, which separates a pad that is not wired from a
threshold that is set too high. A benchmark pinned at 4194303 means the channel
is not being sampled at all.

If a KEY press does nothing, the press is being dropped rather than misread: the
latch in `main.c` retries every pass until the LVGL lock is free, so a dead
button means GPIO18 is not reading, not that the UI is stuck.

## 7. Now add CAN

Wire the SN65HVD230 to the 2x8 header (P1):

| P1 pin | Signal | To transceiver |
|---|---|---|
| 1 | 3V3 | Vcc - **3.3 V, never 5 V** |
| 3 | GND | GND, and Rs to GND for high-speed mode |
| **13** | **GPIO17** | **D / CTX** (driver input - the pin we drive) |
| **11** | **GPIO3** | **R / CRX** (receiver output - the pin we read) |

Pins 7 and 9 are the touch pads, not CAN. And `D` is an input while `R` is an
output: swap them and two push-pull drivers meet on one net, which is how the
first transceiver died during bring-up.

Termination and the rest of the bus: [wiring.md](wiring.md). The bus-off spam
from step 5 should stop the moment something ACKs.

Then follow [bring-up-usb-can.md](bring-up-usb-can.md) from step 4 - the
protocol validation is identical, and if you have already done it on the laptop
the decode table is already correct. Build with the raw logger
(`idf.py -C port/esp32 menuconfig` -> Daly BMS monitor -> raw CAN frame logger)
if you want the same frame dump on the device.

## 8. When the panel works

Photograph it next to a simulator screenshot of the same page. The two should be
pixel-identical: both quantise through `ui_px_is_paper()` from the same RGB565
render, so any difference is a real bug in the flush path rather than a
rendering difference.

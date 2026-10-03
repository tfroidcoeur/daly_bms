# Reference hardware

Specs for the setup this project is developed and tested on: a Waveshare
ESP32-S3-RLCD-4.2 and an SN65HVD230 transceiver on a bus with three Daly BMS
units. Other boards work too - see [../porting.md](../porting.md) - and the
notes here show what to look up for yours.

| Document | Contents |
|---|---|
| [esp32-s3-rlcd-4.2.md](esp32-s3-rlcd-4.2.md) | The compute + display board: full spec and GPIO map |
| [sn65hvd230.md](sn65hvd230.md) | CAN transceiver: pinout, modes, termination rules |
| [daly-can-protocol.md](daly-can-protocol.md) | Daly CAN frame format and command table |
| [wiring.md](wiring.md) | How the CAN bus is wired and brought up |
| [bring-up-usb-can.md](bring-up-usb-can.md) | First contact from the laptop: USB-CAN adapter straight onto the packs, no ESP32 |
| [bring-up-board.md](bring-up-board.md) | First flash of the real board, and how to read a panel that does not light up |

## Parts

| Part | Role |
|---|---|
| **Waveshare ESP32-S3-RLCD-4.2** | Compute + display |
| **SN65HVD230** CAN transceiver breakout | ESP32 to CAN physical layer |

## Two findings that shaped the design

1. **The screen is not a touchscreen.** Waveshare's own documentation states
   *Touch Panel: None*. It is a 4.2" *reflective* LCD, not a capacitive touch
   panel. Navigation uses two capacitive pads on GPIO1 and GPIO2, with the
   onboard **KEY button (GPIO18)** still working alongside them.
   (The Zephyr board documentation mentions "capacitive touch" — that refers to
   the ESP32-S3 SoC's built-in touch-sensor peripheral, not a panel on this board.)
2. **The ESP32-S3 has exactly one TWAI (CAN) controller**, and one is enough:
   the three BMS units share one bus and are told apart by their Daly board
   address, which must be set to `0x01` / `0x02` / `0x03` first.

## Datasheets

Run [`fetch-datasheets.sh`](datasheets/fetch-datasheets.sh) to download the PDFs
into `datasheets/` (they are gitignored — several MB each).

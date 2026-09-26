# Hardware reference

Collected specs for the Daly BMS monitor (`lier_bms_scherm`).

Everything here was researched on 2026-09-02 and written down so the wiring and
firmware work does not depend on re-finding web pages.

| Document | Contents |
|---|---|
| [esp32-s3-rlcd-4.2.md](esp32-s3-rlcd-4.2.md) | The compute + display board: full spec and GPIO map |
| [sn65hvd230.md](sn65hvd230.md) | CAN transceiver: pinout, modes, termination rules |
| [daly-can-protocol.md](daly-can-protocol.md) | Daly CAN frame format and command table |
| [wiring.md](wiring.md) | How the CAN bus is wired and brought up |
| [bring-up-usb-can.md](bring-up-usb-can.md) | First contact from the laptop: USB-CAN adapter straight onto the packs, no ESP32 |
| [bring-up-board.md](bring-up-board.md) | First flash of the real board, and how to read a panel that does not light up |

## What was ordered

| ASIN | Item | Price | Role |
|---|---|---|---|
| `B0FLPTC5NM` | Hoite **SN65HVD230** CAN transceiver breakout, pack of 5 | — | ESP32 to CAN physical layer |
| `B0GLPHR1ZX` | **Waveshare ESP32-S3-RLCD-4.2** (sold as UeeKKoo `ESP32-S3-RLCD-4.2-EN`) | EUR 33,59 | Compute + display |

Order links:

- <https://www.amazon.com.be/dp/B0FLPTC5NM> (listed on amazon.de as
  "Hoite SN65HVD-230 CAN Bus Module VP230 Communication Module Can Bus Transceiver
  Breakout Board Pack of 5")
- <https://www.amazon.com.be/dp/B0GLPHR1ZX>

Delivery estimate for the display board: **19-24 September 2026**.

## Two findings that shaped the design

1. **The screen is not a touchscreen.** Waveshare's own documentation states
   *Touch Panel: None*. It is a 4.2" *reflective* LCD, not a capacitive touch
   panel. Navigation uses two capacitive pads on GPIO1 and GPIO2, with the
   onboard **KEY button (GPIO18)** still working alongside them.
   (The Zephyr board documentation mentions "capacitive touch" — that refers to
   the ESP32-S3 SoC's built-in touch-sensor peripheral, not a panel on this board.)
2. **The ESP32-S3 has exactly one TWAI (CAN) controller.** Three BMS units
   therefore share one bus and are told apart by their Daly board address, which
   must be set to `0x01` / `0x02` / `0x03` first. Only **one** of the five
   transceivers is needed; the rest are spares.

## Datasheets

Run [`fetch-datasheets.sh`](datasheets/fetch-datasheets.sh) to download the PDFs
into `datasheets/` (they are gitignored — several MB each).

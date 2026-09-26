# CAN wiring plan

How the SN65HVD230 transceiver connects the ESP32-S3-RLCD-4.2 to three Daly BMS
units.

## Topology

One linear bus, four nodes, common ground.

```
   ESP32-S3-RLCD-4.2                Daly #1      Daly #2      Daly #3
   +----------------+               addr 0x01    addr 0x02    addr 0x03
   |                |                   |            |            |
   |  GPIO_TX ------+---> D  +--------+ |            |            |
   |  GPIO_RX <-----+---- R  |SN65HVD | |            |            |
   |  3V3 ----------+---> Vcc|  230   | |            |            |
   |  GND ----------+---> GND|        | |            |            |
   +----------------+        +-+----+-+ |            |            |
                          CANH |    | CANL           |            |
   [120R]----------------------+----+-----+----------+------------+---[120R]
   end of bus                                                   end of bus
                    (GND run alongside the twisted pair)
```

Because the three packs are wired in **parallel**, they already share battery
negative. There is one common ground reference and no isolation is needed between
BMS units.

## Node budget

Only **one** transceiver is used. The pack of five leaves four spares.

## Pin assignment

**Resolved from the schematic.** The 2 x 8 header (P1) brings out only six GPIOs,
of which three are freely usable: **GPIO1, GPIO2, GPIO17**. Everything else on the
header is power, USB, UART0, I2C, or an already-committed pin.

![P1 header](img/p1-header.png)

Touch gets first pick and CAN takes what is left. The constraint only runs one
way: the touch channels are wired to GPIO1..GPIO14 in silicon, while TWAI routes
through the GPIO matrix and works on any pin. With the display behind a sealed
front, two capacitive pads replace the onboard KEY button, and that claims both
low-numbered pins.

| Signal | P1 pin | GPIO |
|---|---|---|
| CAN TX -> transceiver `D` / `CTX` | **13** | **GPIO17** |
| CAN RX <- transceiver `R` / `CRX` | **11** | **GPIO3** |
| 3V3 -> transceiver `Vcc` | **1** | - |
| GND -> transceiver `GND` | **3** | - |
| touch pad: next page | **7** | **GPIO1** |
| touch pad: cell detail | **9** | **GPIO2** |

That uses all four usable header pins, with nothing spare.

GPIO3 is nominally the JTAG source-select strap, but that function only exists
once the `JTAG_SEL_ENABLE` eFuse is burned, which it is not from the factory. It
carries **RX rather than TX** deliberately, so the pin sits at a defined level
during boot: the transceiver's `R` output idles high (recessive).

Do not use GPIO0 (pin 5, BOOT strap) or GPIO18 (pin 15, KEY button).

**`D`/`CTX` is an input to the transceiver and takes our TX; `R`/`CRX` is its
output and feeds our RX.** Wired the other way round, two push-pull drivers end
up on one net - which during bring-up destroyed a transceiver and cost most of
an evening to find, because every symptom pointed at the bus instead.

For the full P1 pinout see
[esp32-s3-rlcd-4.2.md](esp32-s3-rlcd-4.2.md#the-2-x-8-expansion-header-p1).

## Bus parameters

| | |
|---|---|
| Bit rate | 250 kbit/s |
| Frame format | 29-bit extended |
| Termination | 120 ohm at each of the two physical ends, and nowhere else |
| Cable | Twisted pair for CANH/CANL, ground wire alongside |
| Stubs | As short as practical - under about 30 cm |

Check termination with a multimeter before powering anything: across the assembled,
unpowered bus, CANH-CANL should read about **60 ohm**. See
[sn65hvd230.md](sn65hvd230.md#termination).

## Safety during development

With USB-C plugged into the laptop, laptop ground becomes tied to pack negative
through the CAN ground. On a live battery bank that is a real hazard and a real
route to a dead laptop.

Either:

- run the board from its 18650 (or another floating supply) whenever it is
  connected to the live bank, **or**
- use a USB isolator between laptop and board.

Do the first bring-up steps on the bench, off the bank, where this does not arise.

## Bring-up order

Work up in stages. Do not connect three BMS units to a live bank as the first test.

### 1. Bench loopback

ESP32 + transceiver + a second CAN node (USB-CAN adapter, or a second ESP32 with
another transceiver) on a short terminated pair. Prove frames go both ways at
250 kbit/s. This validates the transceiver wiring, the Rs mode, the GPIO choice,
and the bit timing in isolation.

If the bus is silent here, check in this order: Rs floating or tied high; TX/RX
swapped; only one terminator fitted; wrong bit rate.

### 2. One BMS, raw logging

Connect a single Daly, still off the main bank if possible. Flash the raw frame
logger build and dump every received identifier and payload over USB serial.

Confirm the unit answers `0x18900140` with source `0x18904001`. Then check every
field layout in [daly-can-protocol.md](daly-can-protocol.md) against Daly's own app
running on the same pack - particularly the 30000 current offset, the 40
temperature offset, and the multi-frame `0x95` cell ordering. **Fix the parser
before trusting the UI.**

### 3. Re-address packs 2 and 3

With Daly's Windows **BmsMonitor** tool, set the board number of the second unit to
`0x02` and the third to `0x03`. The Smart BMS phone app does not set this
reliably.

Verify each responds on its own address individually, one at a time, before
bussing them together.

### 4. All three together

Bus all three plus the ESP32. Confirm three distinct source addresses reply within
one poll round, and that unplugging one connector greys exactly that pack on screen
within the timeout - and no others.

### 5. On the bank

Move to the live parallel bank, on battery or isolated USB. Cross-check SoC, pack
voltage and current for each pack against Daly's app.

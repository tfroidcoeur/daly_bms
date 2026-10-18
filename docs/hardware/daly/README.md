# Daly BMS documents

The packs use Daly's blue **Smart Active Balance BMS**, 4-24S, 40-500A, sold
as "Daly Balance BMS 4S-24S 40A-500A". The PDFs are gitignored; fetch them
with [`fetch-daly-docs.sh`](fetch-daly-docs.sh).

| File | What it is | Source |
|---|---|---|
| `Daly-Smart-Active-Balance-BMS-User-Manual.pdf` | **The manual for these packs.** 4 pages: models and sizes, installation, sleep timer, specifications, port pinouts, wiring diagram. | dalyelec.com, [DALY Smart BMS Product Manual](https://www.dalyelec.com/software_jhbe.html) |
| `Daly-CAN-Communications-Protocol-V1.0.pdf` | CAN protocol, 2019-06-11. What [../daly-can-protocol.md](../daly-can-protocol.md) is built on. | Reseller mirror (robu.in), Daly does not host it |
| `Daly-UART-485-Communications-Protocol-V1.2.pdf` | UART/RS485 protocol, 2020-12-22. Same payloads as CAN. | Reseller mirror (robu.in) |

Also from Daly, as web pages rather than PDFs:

- [DALY Smart BMS Control Strategy](https://www.dalybms.com/news/daly-smart-bms-control-strategy/):
  sleep and wake-up conditions, including CAN wake-up.
- [Technical support / downloads](https://www.dalybms.com/technical-support-2/)
  and [PC software](https://www.dalybms.com/download-pc-software/): BmsMonitor,
  BMSTool and the CAN and UART/485 drivers.

## Activation, sleep and wake-up, from the manual

- **Activation:** connect the accessories first, then plug in the sampling
  cable; "insert the sampling cable into the BMS to activate automatically"
  (section II.1).
- **Sleep:** "When there is no charging or discharging, the BMS will sleep
  after 3600 seconds by default, and will wake up automatically when charging
  and discharging are detected. The sleep time can also be modified through
  the APP or PC. If set to 65535, it means no sleep." (section II.3)
- **Power:** working ≤ 1.25 W, sleep ≤ 0.025 W (section III.1).
- **Wake button:** Bluetooth is built in, so there is no Bluetooth dongle with
  a button. The only input the manual names as one is **UART port pin 4,
  `S1`: "Activate button"**. The LCD, Wi-Fi/GPS and power indicator boards
  plug into this port.
- **Key switch:** the `KEY` port has `KEY+` on pin 1 ("Key switch positive")
  and `KEY-` on pin 4 ("Key switch negative"); pins 2 and 3 are unused. The
  manual says nothing more. See below.

The manual does not mention waking up over CAN. Only the Control Strategy
article does.

## The key switch

The manual stops at the pinout. What the switch does comes from Daly's PC
tools and two web sources:

- **It is configurable.** Both of Daly's PC tools have a **"Key logic"**
  parameter with five choices. [BmsMonitor
  V2.4.16](https://www.dalybms.com/uploads/DalyBmsMonitorV2.4.16.zip) labels
  them, in order:

  | BmsMonitor | BMSTool V1.14.79 | Reading |
  |---|---|---|
  | Disable | disable | key switch does nothing |
  | Dmos and sleep | discharge and sleep | switch off: discharge MOSFET off, BMS sleeps |
  | Dmos | discharge | switch off: discharge MOSFET off |
  | Dmos cmos | charge discharge | switch off: both MOSFETs off |
  | Dmos cmos sleep | charge dischr sleep | switch off: both MOSFETs off, BMS sleeps |

  The labels come from the programs' strings; the "Reading" column is
  inferred from them and from the forum report below. BMSTool's status list
  also has two flags for this, "key turn off chg mos" and "key turn off dsg
  mos", next to "comm command turn off chg mos" / "... dsg mos".
- **Default: activation only.** Daly's [Smart BMS news
  page](https://www.dalybms.com/news/smart-bms/) says a customised key switch
  "can control the battery charge and discharge MOS" and "control the
  activation and hibernation of the battery pack". A seller of Daly's key
  switch for this BMS says it only activates the BMS unless the function is
  turned on in the PC software.
- **Field report:** an owner of a new Daly BMS with the same 4-pin JST GH `KEY`
  connector found that only the two outer pins are used. They set "Control
  discharge MOS and sleep" in the Android app; with the switch open, the BMS
  opened its discharge MOSFET and went to sleep.
  ([DIY Solar Forum](https://diysolarforum.com/threads/advice-appreciated-key-switch-wiring-for-the-new-daly-bms-jst-gh-4-pin-connector.98106/))

So with nothing plugged into `KEY`, these packs behave as they do now, unless
someone has set "Key logic" to something other than Disable. That is worth
checking in BmsMonitor before relying on it. If the field report is right that
an open switch means "off", a MOSFET option with no switch fitted would keep
the MOSFETs off.

### Wake-up sources

BmsMonitor also has a **"WakeUpMethod"** group with five indicators: key,
button, RS485, **CAN** and current wake-up. That suggests each BMS records
which wake sources are enabled, CAN among them. It is not clear from the
program whether these can be changed or only displayed. Reading them from a
pack in BmsMonitor would settle whether CAN wake-up is on for these packs.

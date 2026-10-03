/*
 * Daly smart BMS CAN protocol: identifier construction and frame decoding.
 *
 * Pure functions over plain buffers. No I/O, no allocation, no platform headers,
 * so this compiles and unit-tests on the host exactly as it runs on the ESP32.
 *
 * See docs/hardware/daly-can-protocol.md, which now carries Daly's own "CAN
 * Communications Protocol V1.0" alongside the community reverse-engineering
 * this was built from. The two agree on everything except the 0x95/0x96 frame
 * numbering and bytes 5-7 of 0x94; both disagreements are handled here rather
 * than decided, and both still want checking against real hardware.
 */
#ifndef DALY_PROTO_H
#define DALY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bms_model.h"

/* Identifier layout: 0x18 | cmd | dest | src */
#define DALY_PRIO       0x18u
#define DALY_HOST_ADDR  0x40u

typedef enum {
    DALY_CMD_SOC         = 0x90, /* pack voltage, current, SoC          */
    DALY_CMD_CELL_MINMAX = 0x91, /* highest/lowest cell + indices       */
    DALY_CMD_TEMP_MINMAX = 0x92, /* highest/lowest temperature          */
    DALY_CMD_MOS         = 0x93, /* MOSFET state, remaining capacity    */
    DALY_CMD_STATUS      = 0x94, /* cell/sensor counts, maybe cycles    */
    DALY_CMD_CELL_VOLTS  = 0x95, /* per-cell voltages    (multi-frame)  */
    DALY_CMD_CELL_TEMPS  = 0x96, /* per-sensor temps     (multi-frame)  */
    DALY_CMD_BALANCE     = 0x97, /* balancing bitfield                  */
    DALY_CMD_FAULTS      = 0x98, /* alarm/fault flags                   */
} daly_cmd_t;

/* Build the extended identifier for a request to `bms_addr`. */
uint32_t daly_request_id(daly_cmd_t cmd, uint8_t bms_addr);

/* Build the identifier a BMS at `bms_addr` answers `host_addr` with. */
uint32_t daly_response_id(uint8_t cmd, uint8_t bms_addr, uint8_t host_addr);

/*
 * Split a received extended identifier.
 *
 * Returns false unless this is a well-formed BMS response addressed to us, i.e.
 * priority 0x18 and destination DALY_HOST_ADDR. `cmd` and `src_addr` are only
 * written on success.
 */
bool daly_decode_id(uint32_t id, uint8_t *cmd, uint8_t *src_addr);

/*
 * Apply an 8-byte response payload to a pack.
 *
 * Returns true if the frame was understood and something in `pack` changed.
 * Unknown commands and malformed multi-frame indices return false and leave the
 * pack untouched - a corrupt frame must never poison good data.
 *
 * Multi-frame commands (0x95, 0x96) accumulate into a scratch buffer and only
 * commit to the visible fields once every expected frame has arrived, so a
 * dropped frame yields no update rather than a half-written cell array.
 *
 * Those two also learn from the traffic whether the pack numbers its frames
 * from 0 or from 1, since the sources disagree and nothing in a frame says.
 * A pack numbering from 1 therefore spends its first burst of each being
 * identified, and shows cells one polling round later than it otherwise would.
 */
bool daly_apply_frame(bms_pack_t *pack, uint8_t cmd, const uint8_t data[8]);

/*
 * 0x98 fault bits, by position: byte * 8 + bit, over the seven flag bytes.
 *
 * Names are plain-language renderings of Daly's own protocol document V1.0;
 * dbus-serialbattery groups the bytes identically and agrees bit for bit where
 * it decodes them. In bytes 0-3 the bits come in pairs, level 1 then level 2:
 * level 1 is Daly's warning, level 2 its protection trip. Bytes 4-6 are
 * hardware faults and count as a trip.
 */

/* Name of one fault bit, or NULL where Daly marks it reserved. Both bits of a
 * level pair share a name; daly_fault_is_trip() tells them apart. */
const char *daly_fault_name(uint8_t byte, uint8_t bit);

/* True if the bit means the BMS has acted rather than merely warned. */
bool daly_fault_is_trip(uint8_t byte, uint8_t bit);

/*
 * Choose the one bit worth a line: the first trip in wire order, else the first
 * warning. A reserved bit counts as a trip - an unknown fault is not a reason
 * to relax. Returns how many bits are set in all; `*pos` is written only if
 * that is not zero.
 */
uint8_t daly_fault_worst(const uint8_t alarms[BMS_ALARM_BYTES], uint8_t *pos);

/* One short line for the fault at `pos`: "cell voltage high, tripped",
 * "EEPROM fault", or "fault byte 3 bit 4" for a bit with no name. */
void daly_fault_describe(uint8_t pos, char *buf, size_t n);

/*
 * Which way Daly's current points once the 30000 bias is removed: +1 if above
 * the bias is charging, -1 if above it is discharging.
 *
 * Daly's own document does not say, and the drivers in circulation disagree -
 * dbus-serialbattery divides by -10 and ships an invert switch, maland16's
 * library reads it the other way. So it is a setting, settled at bring-up: put
 * a load on the pack, and the current must go negative. If it goes positive,
 * set CONFIG_BMS_INVERT_CURRENT on the device, or -DBMS_INVERT_CURRENT=ON on
 * the host.
 *
 * Whatever the wire does, the model's convention is fixed: positive pack_ma is
 * charging. And nothing safety-relevant depends on it either way - the
 * charging-below-freezing rule reads the BMS's own state from 0x93.
 */
#ifndef DALY_CURRENT_SIGN
#define DALY_CURRENT_SIGN 1
#endif

/* Scaling helpers, exposed for tests and for the frame logger. */
static inline int32_t daly_decode_current_ma(uint16_t raw)
{
    /* 0.1 A resolution, biased by 30000 so that either direction fits. */
    return ((int32_t)raw - 30000) * 100 * DALY_CURRENT_SIGN;
}

static inline int8_t daly_decode_temp_c(uint8_t raw)
{
    return (int8_t)((int16_t)raw - 40);
}

/*
 * The inverses, for the virtual BMS. The current goes out with the same sign
 * convention the real packs are read with, so whatever reads the virtual pack
 * needs the same setting as it would for any one of them.
 */
static inline uint16_t daly_encode_current_raw(int32_t ma)
{
    /* Nearest 0.1 A, half away from zero. */
    const int32_t units = (ma >= 0 ? ma + 50 : ma - 50) / 100;
    int32_t raw = 30000 + units * DALY_CURRENT_SIGN;
    if (raw < 0) {
        raw = 0;
    } else if (raw > 0xFFFF) {
        raw = 0xFFFF;
    }
    return (uint16_t)raw;
}

static inline uint8_t daly_encode_temp_raw(int8_t c)
{
    return c < -40 ? 0 : (uint8_t)((int16_t)c + 40);
}

#endif /* DALY_PROTO_H */

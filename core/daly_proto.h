/*
 * Daly smart BMS CAN protocol: identifier construction and frame decoding.
 *
 * Pure functions over plain buffers. No I/O, no allocation, no platform headers,
 * so this compiles and unit-tests on the host exactly as it runs on the ESP32.
 *
 * See docs/hardware/daly-can-protocol.md. The field layouts there are from
 * community reverse-engineering and must be validated against real hardware.
 */
#ifndef DALY_PROTO_H
#define DALY_PROTO_H

#include <stdbool.h>
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
    DALY_CMD_STATUS      = 0x94, /* cell/sensor counts, cycles          */
    DALY_CMD_CELL_VOLTS  = 0x95, /* per-cell voltages    (multi-frame)  */
    DALY_CMD_CELL_TEMPS  = 0x96, /* per-sensor temps     (multi-frame)  */
    DALY_CMD_BALANCE     = 0x97, /* balancing bitfield                  */
    DALY_CMD_FAULTS      = 0x98, /* alarm/fault flags                   */
} daly_cmd_t;

/* Build the extended identifier for a request to `bms_addr`. */
uint32_t daly_request_id(daly_cmd_t cmd, uint8_t bms_addr);

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
 */
bool daly_apply_frame(bms_pack_t *pack, uint8_t cmd, const uint8_t data[8]);

/* Scaling helpers, exposed for tests and for the frame logger. */
static inline int32_t daly_decode_current_ma(uint16_t raw)
{
    /* 0.1 A resolution, biased by 30000 so that discharge can go negative. */
    return ((int32_t)raw - 30000) * 100;
}

static inline int8_t daly_decode_temp_c(uint8_t raw)
{
    return (int8_t)((int16_t)raw - 40);
}

#endif /* DALY_PROTO_H */

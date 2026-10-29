/*
 * A virtual BMS: the three parallel packs presented as one battery, in the
 * Victron CAN-bus BMS protocol.
 *
 * The packs themselves speak Daly; the winch controller reads a single battery
 * the way a Victron system does. So the bank goes out as a pair of 11-bit
 * broadcasts once a second - nobody asks, Victron is push-only:
 *
 *   0x355  SoC (1 %), SoH (1 %), high-res SoC (0.01 %)          un16, LE
 *   0x356  voltage (0.01 V), current (0.1 A, + = charging),     sn16, LE
 *          temperature (0.1 C, the warmest sensor; left off when unknown)
 *
 * Daly reports no state of health, so SoH is VBMS_SOH_PCT. Victron advises one
 * update a second and stops after three without.
 *
 * Like the poller it has no I/O. The caller transmits whatever it returns:
 *
 *     n = vbms_tick(&v, &model, now, frames);
 *     for (i = 0; i < n; i++) send(frames[i]);
 *
 * How the packs combine, given that they are wired in parallel:
 *
 *   voltage      mean                  they share terminals
 *   current      sum                   each carries its share of the load
 *   SoC          mean
 *   remaining    sum
 *   cell min/max worst across packs    never averaged
 *   cell array   per position, the pack whose cell strays furthest from the
 *                bank mean - so the array's own extremes are the bank's
 *   temperatures every sensor of every pack, end to end, up to 16
 *   MOSFETs      on only if on in every pack
 *   faults       OR across packs, plus Daly's "communication failure" bit
 *                while any pack is offline
 *
 * Only 0x355 and 0x356 are sent today; vbms_aggregate() holds the rest for
 * when the controller wants more of the protocol.
 *
 * Only online packs count, and a field nobody has reported yet is not
 * invented: with no pack online, or no SoC reported yet, nothing is sent, so
 * the controller times the battery out rather than act on stale numbers.
 */
#ifndef VIRTUAL_BMS_H
#define VIRTUAL_BMS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_model.h"
#include "poller.h"

#define VBMS_ID_SOC     0x355u
#define VBMS_ID_VIT     0x356u
#define VBMS_PERIOD_MS  1000u
#define VBMS_SOH_PCT    100
#define VBMS_MAX_FRAMES 2

/* 0x98 byte 5 bit 6, Daly's "communication failure". */
#define VBMS_COMM_FAULT_BYTE 5
#define VBMS_COMM_FAULT_BIT  6

typedef struct {
    bool     enabled;
    bool     started;     /* something has been sent; last_tx_ms is real */
    uint32_t last_tx_ms;
    uint32_t sent;        /* broadcasts sent, for diagnostics */
} vbms_t;

void vbms_init(vbms_t *v, bool enabled);

/*
 * Combine the online packs into one. Exposed so tests and the host monitor can
 * see exactly what the virtual BMS is about to say; validity flags in `out`
 * say which fields hold something.
 */
void vbms_aggregate(const system_model_t *m, bms_pack_t *out);

/* The bank as 0x355 and 0x356, standard frames. Returns 0 when it has no SoC. */
uint8_t vbms_encode(const bms_pack_t *agg, can_frame_out_t out[VBMS_MAX_FRAMES]);

/*
 * Call every pass of the loop. Returns how many frames of `out` to send now:
 * 2 once a period, 0 otherwise and whenever there is nothing to report.
 */
uint8_t vbms_tick(vbms_t *v, const system_model_t *m, uint32_t now_ms,
                  can_frame_out_t out[VBMS_MAX_FRAMES]);

#endif /* VIRTUAL_BMS_H */

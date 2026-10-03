/*
 * A virtual Daly BMS: the three parallel packs presented as one.
 *
 * Something that only knows how to read a single Daly BMS - another dashboard,
 * say - polls this address exactly as it would poll a real pack, and gets the
 * bank back. Same identifiers, same payloads, same 1-based multi-frame bursts as
 * the firmware this project has met.
 *
 * Like the poller it has no I/O. The caller hands it every received frame and
 * transmits whatever it returns:
 *
 *     n = vbms_on_frame(&v, &model, id, data, len, frames);
 *     for (i = 0; i < n; i++) send(frames[i]);
 *
 * How the packs combine, given that they are wired in parallel:
 *
 *   voltage      mean                  they share terminals
 *   current      sum                   each carries its share of the load
 *   SoC          mean
 *   remaining    sum
 *   cell min/max worst across packs    0x91 is never averaged
 *   cell array   per position, the pack whose cell strays furthest from the
 *                bank mean - so the array's own extremes are the bank's
 *   temperatures every sensor of every pack, end to end, up to 16
 *   MOSFETs      on only if on in every pack
 *   faults       OR across packs, plus Daly's "communication failure" bit
 *                while any pack is offline
 *
 * Only online packs count, and a field nobody has reported yet is not invented:
 * a command with nothing behind it goes unanswered, which is what a real pack
 * that has not got the data does too. With every pack offline the virtual BMS
 * falls silent altogether, so the reader shows it offline rather than stale.
 */
#ifndef VIRTUAL_BMS_H
#define VIRTUAL_BMS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_model.h"
#include "poller.h"

/* The longest answer: a 48-cell 0x95 burst. */
#define VBMS_MAX_FRAMES 16

/* 0x98 byte 5 bit 6, Daly's "communication failure". */
#define VBMS_COMM_FAULT_BYTE 5
#define VBMS_COMM_FAULT_BIT  6

typedef struct {
    uint8_t  addr;       /* 0 = disabled */
    uint8_t  life;       /* 0x93's BMS life byte, ours to count */
    uint32_t requests;   /* requests answered, for diagnostics */
} vbms_t;

/*
 * Answer at `addr`. Returns false, and leaves the virtual BMS disabled, for 0,
 * for any of the real packs' addresses and for the host address - each of
 * those would put two talkers on one identifier.
 */
bool vbms_init(vbms_t *v, uint8_t addr);

/*
 * Combine the online packs into one. Exposed so tests and the host monitor can
 * see exactly what the virtual BMS is about to say; validity flags in `out`
 * say which fields hold something.
 */
void vbms_aggregate(const system_model_t *m, bms_pack_t *out);

/*
 * Feed in any received frame. If it is a request to our address, fills `out`
 * with the answer, addressed back to whoever asked, and returns how many frames
 * to send. Returns 0 for anything else, including requests we cannot answer yet.
 */
uint8_t vbms_on_frame(vbms_t *v, const system_model_t *m, uint32_t id,
                      const uint8_t *data, uint8_t len,
                      can_frame_out_t out[VBMS_MAX_FRAMES]);

#endif /* VIRTUAL_BMS_H */

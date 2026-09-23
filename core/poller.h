/*
 * Round-robin poll scheduler for three Daly packs on one shared CAN bus.
 *
 * Deliberately has no threads, no timers and no I/O. The caller drives it:
 *
 *     poller_tick(&p, now_ms, &frame)  -> true if `frame` should be transmitted
 *     poller_on_frame(&p, id, data, now_ms)
 *
 * That makes the whole polling policy - ordering, spacing, timeouts, staleness -
 * testable on the host with a fake clock and no hardware.
 */
#ifndef POLLER_H
#define POLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_model.h"

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
} can_frame_out_t;

typedef struct {
    /* Gap between consecutive requests. Must leave room for the reply,
     * including the multi-frame 0x95 burst. */
    uint32_t request_gap_ms;
    /* A pack unheard from for this long is marked offline. */
    uint32_t offline_timeout_ms;
    /* Poll the slow-changing commands once every N rounds. */
    uint8_t  slow_divider;
} poller_cfg_t;

typedef struct {
    poller_cfg_t   cfg;
    system_model_t *model;

    uint8_t  pack_idx;      /* which pack we are asking */
    uint8_t  step;          /* index into the command schedule */
    uint32_t round;         /* completed full rounds, drives slow_divider */
    uint32_t next_tx_ms;
    bool     started;
} poller_t;

/*
 * Tuned defaults: a request every 40 ms, a pack offline after 5 s of silence,
 * slow commands every 10th round. Nine commands across three packs at that gap
 * is a full round roughly every 1.1 s.
 */
poller_cfg_t poller_default_cfg(void);

/*
 * Bind a poller to the model it will fill in. The poller does not own `model`
 * and never frees it; the model must outlive the poller.
 */
void poller_init(poller_t *p, system_model_t *model, poller_cfg_t cfg);

/*
 * Advance the schedule. Returns true and fills `out` when it is time to send a
 * request; returns false when there is nothing to do yet.
 */
bool poller_tick(poller_t *p, uint32_t now_ms, can_frame_out_t *out);

/*
 * Feed a received frame in. Returns true if it was a Daly response for one of
 * our packs. Marks that pack online and stamps last_seen_ms.
 */
bool poller_on_frame(poller_t *p, uint32_t id, const uint8_t *data, uint8_t len,
                     uint32_t now_ms);

#endif /* POLLER_H */

#include "poller.h"

#include <stddef.h>

#include "daly_proto.h"

/*
 * The per-pack schedule. Fast commands run every round; slow ones run once per
 * `slow_divider` rounds, because cell count, cycle count and fault flags do not
 * change between seconds and the bus is shared by three packs.
 */
typedef struct {
    uint8_t cmd;
    bool    slow;
} sched_step_t;

static const sched_step_t k_schedule[] = {
    { DALY_CMD_STATUS,      true  }, /* first: cell/temp counts gate 0x95/0x96 */
    { DALY_CMD_SOC,         false },
    { DALY_CMD_CELL_MINMAX, false },
    { DALY_CMD_TEMP_MINMAX, false },
    { DALY_CMD_MOS,         false },
    { DALY_CMD_CELL_VOLTS,  false },
    { DALY_CMD_CELL_TEMPS,  true  },
    { DALY_CMD_BALANCE,     true  },
    { DALY_CMD_FAULTS,      true  },
};

#define SCHEDULE_LEN ((uint8_t)(sizeof k_schedule / sizeof k_schedule[0]))

poller_cfg_t poller_default_cfg(void)
{
    poller_cfg_t c = {
        /* 40 ms is comfortably more than a 0x95 burst at 250 kbit/s, and gives
         * a full three-pack round in well under a second. */
        .request_gap_ms     = 40,
        .offline_timeout_ms = 5000,
        .slow_divider       = 10,
    };
    return c;
}

void poller_init(poller_t *p, system_model_t *model, poller_cfg_t cfg)
{
    p->cfg        = cfg;
    /* 0 reads as "never skip", not as a division by zero. */
    if (p->cfg.slow_divider == 0) {
        p->cfg.slow_divider = 1;
    }
    p->model      = model;
    p->pack_idx   = 0;
    p->step       = 0;
    p->round      = 0;
    p->next_tx_ms = 0;
    p->started    = false;
}

/* Advance (pack, step), rolling over into the next round. */
static void schedule_advance(poller_t *p)
{
    p->step++;
    if (p->step < SCHEDULE_LEN) {
        return;
    }
    p->step = 0;
    p->pack_idx++;
    if (p->pack_idx >= BMS_PACK_COUNT) {
        p->pack_idx = 0;
        p->round++;
    }
}

bool poller_tick(poller_t *p, uint32_t now_ms, can_frame_out_t *out)
{
    bms_model_age(p->model, now_ms, p->cfg.offline_timeout_ms);

    if (!p->started) {
        p->started    = true;
        p->next_tx_ms = now_ms;
    } else if ((int32_t)(now_ms - p->next_tx_ms) < 0) {
        return false;
    }

    /* Skip slow steps except on their round; skip cell/temp reads until the
     * 0x94 status reply has told us how many cells and sensors there are. */
    for (uint8_t guard = 0; guard < SCHEDULE_LEN * BMS_PACK_COUNT; guard++) {
        const sched_step_t *s    = &k_schedule[p->step];
        const bms_pack_t   *pack = &p->model->pack[p->pack_idx];

        bool skip = false;
        if (s->slow && (p->round % p->cfg.slow_divider) != 0) {
            skip = true;
        }
        if (s->cmd == DALY_CMD_CELL_VOLTS && pack->cell_count == 0) {
            skip = true;
        }
        if (s->cmd == DALY_CMD_CELL_TEMPS && pack->temp_count == 0) {
            skip = true;
        }

        if (!skip) {
            out->id     = daly_request_id((daly_cmd_t)s->cmd, pack->addr);
            out->len    = 8;
            for (int i = 0; i < 8; i++) {
                out->data[i] = 0;
            }
            schedule_advance(p);
            p->next_tx_ms = now_ms + p->cfg.request_gap_ms;
            return true;
        }
        schedule_advance(p);
    }

    /* Everything in this pass was skipped; try again on the next tick. */
    p->next_tx_ms = now_ms + p->cfg.request_gap_ms;
    return false;
}

bool poller_on_frame(poller_t *p, uint32_t id, const uint8_t *data, uint8_t len,
                     uint32_t now_ms)
{
    uint8_t cmd, src;

    if (len < 8 || !daly_decode_id(id, &cmd, &src)) {
        return false;
    }
    bms_pack_t *pack = bms_model_by_addr(p->model, src);
    if (pack == NULL) {
        return false;
    }

    /*
     * Liveness is credited for any well-formed response from a known pack, even
     * one whose payload we reject - the pack is demonstrably talking to us.
     */
    pack->online       = true;
    pack->last_seen_ms = now_ms;
    pack->frames_rx++;

    daly_apply_frame(pack, cmd, data);
    return true;
}

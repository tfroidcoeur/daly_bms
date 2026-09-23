#include "bms_model.h"

#include <string.h>

void bms_model_init(system_model_t *m)
{
    memset(m, 0, sizeof *m);
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        m->pack[i].addr = (uint8_t)(i + 1); /* Daly addresses 0x01..0x03 */
    }
}

bms_pack_t *bms_model_by_addr(system_model_t *m, uint8_t addr)
{
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        if (m->pack[i].addr == addr) {
            return &m->pack[i];
        }
    }
    return NULL;
}

void bms_model_age(system_model_t *m, uint32_t now_ms, uint32_t timeout_ms)
{
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            continue;
        }
        /* Unsigned subtraction, so this stays correct across a 32-bit wrap. */
        if ((uint32_t)(now_ms - p->last_seen_ms) > timeout_ms) {
            p->online = false;
            /*
             * Drop any half-assembled multi-frame set. Without this, the burst
             * that resumes after the outage can complete the one interrupted by
             * it, publishing cells from both sides of the gap as one reading.
             */
            memset(p->rx.seen_cells, 0, sizeof p->rx.seen_cells);
            memset(p->rx.seen_temps, 0, sizeof p->rx.seen_temps);
        }
    }
}

uint16_t bms_pack_cell_delta_mv(const bms_pack_t *p)
{
    if (!p->cell_max_mv || p->cell_max_mv < p->cell_min_mv) {
        return 0;
    }
    return (uint16_t)(p->cell_max_mv - p->cell_min_mv);
}

int32_t bms_pack_watts(const bms_pack_t *p)
{
    /* mV * mA = microwatts; scale down in one step to avoid overflow. */
    return (int32_t)(((int64_t)p->pack_mv * p->pack_ma) / 1000000);
}

int32_t bms_summary_watts(const bms_summary_t *s)
{
    /* The packs are in parallel, so bank power is the mean voltage times the
     * summed current - not a sum of per-pack products. */
    return (int32_t)(((int64_t)s->bank_mv * s->total_ma) / 1000000);
}

bms_summary_t bms_model_summary(const system_model_t *m)
{
    bms_summary_t s;
    memset(&s, 0, sizeof s);

    int64_t mv_sum  = 0;
    int64_t soc_sum = 0;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            continue;
        }
        s.online_count++;
        s.total_ma += p->pack_ma;
        mv_sum     += p->pack_mv;
        soc_sum    += p->soc_pct_x10;

        const uint16_t d = bms_pack_cell_delta_mv(p);
        if (d > s.worst_cell_delta_mv) {
            s.worst_cell_delta_mv = d;
        }
        if (p->alarm_active) {
            s.any_alarm = true;
        }
        if (p->charger_present) {
            s.charger_present = true;
        }
        /* Charging wins over discharging: if any pack is taking charge, that is
         * the fact worth surfacing. */
        if (p->charge_state == 1) {
            s.charge_state = 1;
        } else if (p->charge_state == 2 && s.charge_state == 0) {
            s.charge_state = 2;
        }
    }

    if (s.online_count > 0) {
        /* The packs are in parallel, so bank voltage is their mean, not sum. */
        s.bank_mv          = (int32_t)(mv_sum / s.online_count);
        s.mean_soc_pct_x10 = (uint16_t)(soc_sum / s.online_count);
    }
    return s;
}

#include "power.h"

#include <string.h>

/*
 * Elapsed time is computed as an unsigned difference so it stays right across
 * the 32-bit millisecond wrap (about every 49 days).
 */
static uint32_t elapsed(uint32_t now_ms, uint32_t since_ms)
{
    return (uint32_t)(now_ms - since_ms);
}

pwr_cfg_t pwr_default_cfg(void)
{
    pwr_cfg_t c = {
        .local_timeout_ms  = PWR_LOCAL_TIMEOUT_MS,
        .report_timeout_ms = PWR_REPORT_TIMEOUT_MS,
    };
    return c;
}

void pwr_init(pwr_t *p, pwr_cfg_t cfg, remote_wake_t wake, uint32_t now_ms)
{
    memset(p, 0, sizeof *p);
    p->cfg = cfg;

    /* A timer wake is only a check-in; with nothing to report it goes straight
     * back to sleep. Any other wake is a person or a bus that wants the bank
     * awake. */
    if (wake == REMOTE_WAKE_TIMER) {
        p->state = PWR_IDLE;
    } else {
        p->state = PWR_LOCAL;
        p->last_activity_ms = now_ms;
    }
}

void pwr_on_touch(pwr_t *p, uint32_t now_ms)
{
    p->last_activity_ms = now_ms;
    p->state = PWR_LOCAL;
}

void pwr_on_vbms_request(pwr_t *p, uint32_t now_ms)
{
    p->last_activity_ms = now_ms;
    p->state = PWR_LOCAL;
}

void pwr_on_report_cmd(pwr_t *p, uint32_t now_ms)
{
    /* A second command while one is pending must not push the timeout out,
     * or a server that asks often would keep the packs awake forever. */
    if (!p->report_wanted) {
        p->report_since_ms = now_ms;
    }
    p->report_wanted = true;

    if (p->state == PWR_IDLE) {
        p->state = PWR_REPORT;
    }
}

void pwr_on_upload_done(pwr_t *p, uint32_t now_ms)
{
    (void)now_ms;
    p->report_wanted = false;
    p->upload_due = false;

    if (p->state == PWR_REPORT) {
        p->state = PWR_IDLE;
    }
}

bool pwr_snapshot_complete(const system_model_t *m)
{
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *k = &m->pack[i];
        if (!k->online || !k->soc_valid || !k->cells_valid ||
            !k->temps_valid || !k->temp_minmax_valid) {
            return false;
        }
    }
    return true;
}

pwr_out_t pwr_tick(pwr_t *p, uint32_t now_ms, bool snapshot_complete)
{
    pwr_out_t out;
    memset(&out, 0, sizeof out);

    /* Upload once: a complete snapshot, or the timeout with whatever is there.
     * upload_due stops later ticks from asking again until the upload is done. */
    if (p->report_wanted && !p->upload_due &&
        (snapshot_complete ||
         elapsed(now_ms, p->report_since_ms) >= p->cfg.report_timeout_ms)) {
        p->upload_due = true;
        out.upload_now = true;
    }

    /* LOCAL outlasts the timeout while a report is still wanted, so the live
     * screen does not drop to "Zzz" under a report that has not gone out. */
    if (p->state == PWR_LOCAL && !p->report_wanted &&
        elapsed(now_ms, p->last_activity_ms) >= p->cfg.local_timeout_ms) {
        p->state = PWR_IDLE;
    }

    switch (p->state) {
    case PWR_LOCAL:
        out.poll_can = true;
        out.screen_live = true;
        break;
    case PWR_REPORT:
        out.poll_can = true;
        break;
    case PWR_IDLE:
    default:
        out.sleep_now = true;
        break;
    }

    return out;
}

/*
 * When to poll the packs, when to show the live screen, and when to sleep.
 *
 * The point of all of it is to let the Daly packs reach their own sleep (they
 * doze off after an hour without bus traffic) whenever nobody needs the data,
 * and to keep the display's power bank going for months. So the bus is quiet
 * unless one of three things wants data:
 *
 *   LOCAL   someone touched the screen, or a dashboard is asking the virtual
 *           BMS; live UI, polling, until 5 min pass without either.
 *   REPORT  the website asked for a snapshot; polling with the screen left on
 *           "Zzz", until every online pack has a full set or 60 s pass.
 *   IDLE    nothing wants data: CAN quiet, "Zzz" on the screen, waiting for a
 *           check-in, a touch or bus traffic - in deep sleep when the wait is
 *           long enough (remote_wait_is_deep()), awake otherwise.
 *
 * Like the rest of core/ it has no I/O and no clock: the port feeds events
 * and asks pwr_tick() what to do. The check-in schedule itself belongs to
 * remote.c; this machine only hears about its outcome.
 */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_model.h"
#include "remote.h"

#define PWR_LOCAL_TIMEOUT_MS   (5u * 60u * 1000u)
#define PWR_REPORT_TIMEOUT_MS  (60u * 1000u)

typedef enum {
    PWR_IDLE = 0,
    PWR_LOCAL,
    PWR_REPORT,
} pwr_state_t;

typedef struct {
    uint32_t local_timeout_ms;    /* PWR_LOCAL_TIMEOUT_MS; shorter in tests */
    uint32_t report_timeout_ms;   /* PWR_REPORT_TIMEOUT_MS */
} pwr_cfg_t;

typedef struct {
    pwr_cfg_t   cfg;
    pwr_state_t state;
    uint32_t    last_activity_ms;  /* last touch or virtual-BMS request */
    uint32_t    report_since_ms;   /* when REPORT began */
    bool        report_wanted;     /* the server asked; upload not yet done */
    bool        upload_due;        /* pwr_tick() said upload, awaiting done */
} pwr_t;

/* What the port should be doing right now. */
typedef struct {
    bool poll_can;      /* run the poller; false = no requests on the bus */
    bool screen_live;   /* draw the live UI; false = the "Zzz" frame */
    bool upload_now;    /* encode and upload the snapshot, then pwr_on_upload_done() */
    bool sleep_now;     /* nothing wants data: wait for remote_seconds_until_checkin() */
} pwr_out_t;

pwr_cfg_t pwr_default_cfg(void);

/*
 * Start after a boot or a wake. BOOT, TOUCH and CAN start in LOCAL (a cold
 * boot shows live data, so whoever just plugged it in sees it work); TIMER
 * starts in IDLE, where the port checks in before anything else.
 */
void pwr_init(pwr_t *p, pwr_cfg_t cfg, remote_wake_t wake, uint32_t now_ms);

/* A touch pad was pressed. In IDLE or REPORT this switches to LOCAL. */
void pwr_on_touch(pwr_t *p, uint32_t now_ms);

/* A request for the virtual BMS was seen on the bus (answered or not). Same
 * effect as a touch. */
void pwr_on_vbms_request(pwr_t *p, uint32_t now_ms);

/*
 * The server asked for a report. From IDLE this enters REPORT; in LOCAL it
 * stays LOCAL and the upload happens as soon as the data is complete. The
 * caller should clear the model first when the bus has been quiet (IDLE), so
 * nothing stale is reported.
 */
void pwr_on_report_cmd(pwr_t *p, uint32_t now_ms);

/* The upload finished, whatever its result. */
void pwr_on_upload_done(pwr_t *p, uint32_t now_ms);

/*
 * True when every pack is online with a full set: soc_valid, cells_valid,
 * temps_valid and temp_minmax_valid. A pack that never answers is left to the
 * report timeout - it may only be slow to wake.
 */
bool pwr_snapshot_complete(const system_model_t *m);

/*
 * Advance timeouts and say what to do. `snapshot_complete` is
 * pwr_snapshot_complete() for the current model. A report is uploaded once
 * complete, or when report_timeout_ms has passed since it was asked for, with
 * whatever is there. LOCAL drops to IDLE after local_timeout_ms without a
 * touch or a virtual-BMS request, but not while an upload is still wanted.
 */
pwr_out_t pwr_tick(pwr_t *p, uint32_t now_ms, bool snapshot_complete);

#endif /* POWER_H */

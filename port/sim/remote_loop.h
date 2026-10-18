/*
 * sim_headless --server: run the remote-reporting firmware against the bus
 * and a website. See remote_loop.c.
 */
#ifndef REMOTE_LOOP_H
#define REMOTE_LOOP_H

#include <signal.h>
#include <stdint.h>

#include "can_socketcan.h"
#include "remote.h"
#include "virtual_bms.h"

typedef struct {
    const remote_transport_t *transport;   /* NULL: never check in */
    uint32_t local_timeout_ms;             /* 0 = PWR_LOCAL_TIMEOUT_MS */
    uint32_t report_timeout_ms;            /* 0 = PWR_REPORT_TIMEOUT_MS */
    uint32_t min_checkin_s;                /* console floor, 0 = 1 */
    uint32_t live_s;                       /* the console's live interval, 0 = off */
} remote_opts_t;

int remote_loop(can_link_t *link, vbms_t *vbms, const remote_opts_t *o,
                volatile sig_atomic_t *stop);

#endif /* REMOTE_LOOP_H */

/*
 * sim_headless --server: the remote-reporting firmware, on the laptop.
 *
 * The same core/power.c, core/remote.c and core/snapshot_json.c the board runs,
 * against SocketCAN and the website (usually the docker test site). Deep sleep
 * is simulated by simply not polling: what the board loses in deep sleep - the
 * model - this clears on every wake, so the difference does not show.
 *
 * It prints one line per event instead of redrawing a screen, so a test script
 * can follow it: "state LOCAL -> IDLE", "checkin ok cmd=report", "upload 204",
 * "live 204" (live reports while the screen is on, --live S).
 * Typing t + Enter is a touch on the pad.
 */
#include "remote_loop.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bms_model.h"
#include "power.h"
#include "remote.h"
#include "snapshot_json.h"
#include "warnings.h"

#define SIM_FW "1.1.0"

/* As on the board: a live screen's first snapshot waits for all packs, or
 * this long. */
#define LIVE_FIRST_MS 15000

static const char *state_name(pwr_state_t s)
{
    switch (s) {
    case PWR_LOCAL:  return "LOCAL";
    case PWR_REPORT: return "REPORT";
    default:         return "IDLE";
    }
}

static const char *wake_name(remote_wake_t w)
{
    switch (w) {
    case REMOTE_WAKE_TIMER: return "timer";
    case REMOTE_WAKE_TOUCH: return "touch";
    case REMOTE_WAKE_CAN:   return "can";
    default:                return "boot";
    }
}

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* A touch typed on stdin, without blocking. */
static bool stdin_touch(void)
{
    char c;
    bool touched = false;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 't') {
            touched = true;
        }
    }
    return touched;
}

typedef struct {
    system_model_t model;
    poller_t       poller;
} bus_t;

/* The bus has been quiet: whatever the model holds is old. Start over so
 * neither the screen nor a report shows it. */
static void bus_restart(bus_t *b)
{
    bms_model_init(&b->model);
    poller_init(&b->poller, &b->model, poller_default_cfg());
}

int remote_loop(can_link_t *link, vbms_t *vbms, const remote_opts_t *o,
                volatile sig_atomic_t *stop)
{
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

    static bus_t bus;
    bus_restart(&bus);

    pwr_cfg_t cfg = pwr_default_cfg();
    if (o->local_timeout_ms) {
        cfg.local_timeout_ms = o->local_timeout_ms;
    }
    if (o->report_timeout_ms) {
        cfg.report_timeout_ms = o->report_timeout_ms;
    }

    const uint32_t t0 = host_millis();
    pwr_t pwr;
    remote_state_t rs;
    remote_wake_t wake = REMOTE_WAKE_BOOT;
    pwr_init(&pwr, cfg, wake, 0);
    remote_state_init(&rs, o->min_checkin_s);
    remote_on_wake(&rs, wake, 0);

    pwr_state_t shown = pwr.state;
    uint32_t vbms_seen = vbms->seen;
    bool sleeping_logged = false;
    uint32_t local_since = 0, last_live = 0;
    bool live_started = false;
    int live_status = 0;
    srand((unsigned)time(NULL));

    printf("remote: %s, state %s\n", o->transport ? "server set" : "no server",
           state_name(pwr.state));
    fflush(stdout);

    while (!*stop) {
        const uint32_t now = host_millis() - t0;
        const uint32_t now_s = now / 1000;
        const bool was_idle = pwr.state == PWR_IDLE;

        if (stdin_touch()) {
            if (was_idle) {
                bus_restart(&bus);
            }
            wake = REMOTE_WAKE_TOUCH;
            pwr_on_touch(&pwr, now);
            remote_on_wake(&rs, wake, now_s);
            printf("touch\n");
        }

        uint32_t id;
        uint8_t data[8], len;
        while (can_link_recv(link, &id, data, &len)) {
            poller_on_frame(&bus.poller, id, data, len, now);
            can_frame_out_t answer[VBMS_MAX_FRAMES];
            const uint8_t n = vbms_on_frame(vbms, &bus.model, id, data, len, answer);
            for (uint8_t i = 0; i < n; i++) {
                can_link_send(link, answer[i].id, answer[i].data, answer[i].len);
            }
        }
        if (vbms->seen != vbms_seen) {
            vbms_seen = vbms->seen;
            if (pwr.state == PWR_IDLE) {
                bus_restart(&bus);
                wake = REMOTE_WAKE_CAN;
                remote_on_wake(&rs, wake, now_s);
                printf("dashboard wake\n");
            }
            pwr_on_vbms_request(&pwr, now);
        }

        if (o->transport && remote_checkin_due(&rs, now_s)) {
            char query[96];
            snprintf(query, sizeof query, "fw=%s&state=%s&wake=%s", SIM_FW,
                     pwr.state == PWR_LOCAL ? "local" : "standby",
                     wake_name(wake));
            bool ok = false;
            const remote_cmd_t cmd =
                remote_checkin(&rs, o->transport, query, now_s, (uint32_t)rand(), &ok);
            const uint32_t next = remote_seconds_until_checkin(&rs, now_s);
            if (next == UINT32_MAX) {
                printf("checkin %s cmd=%s next=never\n", ok ? "ok" : "failed",
                       cmd == REMOTE_CMD_REPORT ? "report" : "none");
            } else {
                printf("checkin %s cmd=%s next=%us\n", ok ? "ok" : "failed",
                       cmd == REMOTE_CMD_REPORT ? "report" : "none", next);
            }
            if (cmd == REMOTE_CMD_REPORT) {
                if (pwr.state == PWR_IDLE) {
                    bus_restart(&bus);
                }
                pwr_on_report_cmd(&pwr, now);
            }
            wake = REMOTE_WAKE_TIMER;
            sleeping_logged = false;
        }

        if (pwr_bank_active(&bus.model)) {
            pwr_on_bank_active(&pwr, now);   /* current flows: keep the screen on */
        }
        const pwr_out_t out =
            pwr_tick(&pwr, now, pwr_snapshot_complete(&bus.model));

        if (pwr.state != shown) {
            if (pwr.state == PWR_LOCAL) {
                local_since  = now;
                live_started = false;
            }
            if (pwr.state == PWR_IDLE) {
                /* The bus goes quiet now; drop what it said, so the first
                 * dashboard request after a long sleep is not answered from
                 * old data. */
                bus_restart(&bus);
            }
            printf("state %s -> %s\n", state_name(shown), state_name(pwr.state));
            shown = pwr.state;
            sleeping_logged = false;
        }

        if (out.poll_can) {
            can_frame_out_t tx;
            if (poller_tick(&bus.poller, now, &tx)) {
                can_link_send(link, tx.id, tx.data, tx.len);
            }
        }

        if (out.upload_now) {
            warning_set_t w;
            warnings_evaluate(&bus.model, &w);
            static char json[SNAPSHOT_JSON_MAX];
            const snapshot_meta_t meta = { rs.req, SIM_FW, now_s };
            const size_t n = snapshot_json_encode(&bus.model, &w, &meta, json, sizeof json);
            const bms_summary_t s = bms_model_summary(&bus.model);
            int status = -1;
            if (n) {
                status = remote_upload(&rs, o->transport, json, n, now_s);
            } else {
                rs.req[0] = '\0';   /* spent either way, as the protocol says */
            }
            printf("upload %d (%u bytes, %u/%u packs online)\n", status,
                   (unsigned)n, s.online_count, BMS_PACK_COUNT);
            pwr_on_upload_done(&pwr, now);
        }

        /* Live reports while the screen is on. */
        const uint32_t live_s = remote_live_interval(&rs, o->live_s);
        if (o->transport && pwr.state == PWR_LOCAL && live_s > 0 &&
            (pwr_snapshot_complete(&bus.model) || now - local_since >= LIVE_FIRST_MS) &&
            (!live_started || now - last_live >= live_s * 1000u)) {
            warning_set_t w;
            warnings_evaluate(&bus.model, &w);
            static char json[SNAPSHOT_JSON_MAX];
            const snapshot_meta_t meta = { NULL, SIM_FW, now_s };
            const size_t n = snapshot_json_encode(&bus.model, &w, &meta, json, sizeof json);
            const int status = n ? remote_live(o->transport, json, n, live_s) : -1;
            /* One line per change, not one every few seconds. */
            if (status != live_status || !live_started) {
                const bms_summary_t s = bms_model_summary(&bus.model);
                printf("live %d (every %us, %u/%u packs online)\n", status, live_s,
                       s.online_count, BMS_PACK_COUNT);
                live_status = status;
            }
            last_live = now;
            live_started = true;
        }

        if (out.sleep_now && !sleeping_logged) {
            const uint32_t wait = remote_seconds_until_checkin(&rs, now_s);
            if (wait == UINT32_MAX) {
                printf("sleep until touch or bus (deep)\n");
            } else {
                printf("sleep %us (%s)\n", wait,
                       remote_wait_is_deep(wait) ? "deep" : "awake");
            }
            sleeping_logged = true;
        }

        fflush(stdout);
        nap_ms(2);
    }
    return 0;
}

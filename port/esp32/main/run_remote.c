/*
 * Normal mode with remote reporting: poll and draw only when someone needs
 * the data, check in with the club website, and deep-sleep the rest of the
 * time so the Daly packs can reach their own sleep.
 *
 * Who decides what:
 *   core/power.c   whether the bus is polled and the screen live (LOCAL,
 *                  REPORT, IDLE) - see power.h for the states
 *   core/remote.c  when to check in, what a reply means, back-off, the cap
 *   this file      the board around them: CAN, the panel, Wi-Fi, deep sleep
 *
 * The network runs in its own task. A check-in can take seconds (Wi-Fi, TLS)
 * and the CAN loop must never wait on it, or packs would look offline and the
 * virtual BMS's broadcasts would stall. The two talk through two queues, and the
 * remote state is only touched by the network task while a job runs.
 *
 * Every boot - a cold start or a wake from deep sleep - comes through here.
 * What must survive deep sleep lives in RTC memory: the remote state, and in
 * net.c the access point to reconnect to.
 */
#include "run_remote.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bms_model.h"
#include "console.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "net.h"
#include "poller.h"
#include "power.h"
#include "remote.h"
#include "sdkconfig.h"
#include "settings.h"
#include "snapshot_json.h"
#include "twai_link.h"
#include "ui.h"
#include "virtual_bms.h"
#include "warnings.h"

static const char *TAG = "remote";

#define UI_UPDATE_MS      500
#define WIFI_TIMEOUT_MS   10000
#define CONSOLE_GATE_MS   10000         /* press within this after reset */
#define NET_TASK_STACK    (12 * 1024)   /* TLS + the cert bundle; see the log */
#define NET_TASK_PRIO     1
/* A live screen's first snapshot waits for every pack's data, or this long
 * if a pack never answers. */
#define LIVE_FIRST_MS     15000

/* The bank as one Victron battery (0x355/0x356) for the winch controller. */
#ifdef CONFIG_BMS_VICTRON
#define VBMS_ENABLED true
#else
#define VBMS_ENABLED false
#endif

/*
 * NOINIT, not DATA: the bootloader reloads RTC data on every reset that is not
 * a deep-sleep wake, which would wipe the day's report count on a crash.
 * remote_state_valid() catches the garbage left by a power-on.
 */
RTC_NOINIT_ATTR static remote_state_t g_rs;

/* Set just before the watchdog below restarts the board over a hung network
 * request; the next boot counts that request as failed. */
#define HUNG_MAGIC 0x48554e47u   /* "HUNG" */
RTC_NOINIT_ATTR static uint32_t g_hung;

/*
 * esp_http_client's timeouts are per socket operation: a server trickling
 * header bytes could keep a request - and the radio - going indefinitely.
 * The body read has its own deadline in net.c; this is the backstop for the
 * rest. A job running longer than this restarts the board.
 */
#define JOB_WATCHDOG_MS   60000

static settings_t g_settings;
static remote_transport_t g_transport;

typedef enum { JOB_CHECKIN, JOB_UPLOAD, JOB_LIVE } job_kind_t;

typedef struct {
    job_kind_t   kind;
    remote_cmd_t cmd;      /* CHECKIN */
    bool         ok;       /* CHECKIN */
    int          status;   /* UPLOAD, LIVE */
    char         req[REMOTE_REQ_LEN + 1];   /* CHECKIN, when cmd is REPORT */
} result_t;

static QueueHandle_t g_jobs;
static QueueHandle_t g_results;

/* Filled by the main loop before it queues a job, and only while no job is
 * outstanding; read by the network task while that job runs. The main loop
 * counts its own jobs (posted minus results), so neither side can see a
 * half-finished hand-over. */
static char   g_query[96];
static char   g_json[SNAPSHOT_JSON_MAX];
static size_t g_json_len;
/* Live snapshots have their own buffer: a report can be encoded into g_json
 * while a live post is still reading this one. */
static char     g_live_json[SNAPSHOT_JSON_MAX];
static size_t   g_live_len;
static uint32_t g_live_every;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Seconds since power-on; keeps counting through deep sleep. */
static uint32_t now_s(void)
{
    return (uint32_t)(esp_rtc_get_time_us() / 1000000ULL);
}

/* What remote.c is handed when there is no network: every request fails, so
 * the check-in is counted as failed and backs off like any other. */
static int no_network(void *ctx, const char *method, const char *path,
                      const char *body, size_t body_len,
                      char *resp, size_t resp_cap, size_t *resp_len)
{
    (void)ctx; (void)method; (void)path; (void)body; (void)body_len;
    (void)resp; (void)resp_cap;
    *resp_len = 0;
    return REMOTE_ERR_TRANSPORT;
}

static void net_task(void *arg)
{
    (void)arg;
    const remote_transport_t offline = { no_network, NULL };

    for (;;) {
        job_kind_t job;
        xQueueReceive(g_jobs, &job, portMAX_DELAY);

        result_t r = { .kind = job };
        if (job == JOB_CHECKIN) {
            const bool up = net_up(WIFI_TIMEOUT_MS);
            r.cmd = remote_checkin(&g_rs, up ? &g_transport : &offline, g_query,
                                   now_s(), esp_random(), &r.ok);
            if (r.cmd == REMOTE_CMD_REPORT) {
                memcpy(r.req, g_rs.req, sizeof r.req);
            }
            const uint32_t next = remote_seconds_until_checkin(&g_rs, now_s());
            ESP_LOGI(TAG, "check-in %s, %s, next in %s%" PRIu32 " s",
                     r.ok ? "ok" : (up ? "failed" : "no Wi-Fi"),
                     r.cmd == REMOTE_CMD_REPORT ? "report" : "none",
                     next == UINT32_MAX ? "never " : "", next == UINT32_MAX ? 0 : next);
        } else if (job == JOB_LIVE) {
            /* Out of Wi-Fi range while winching is normal: the attempt fails
             * quietly and the next one comes at the usual interval. */
            r.status = net_up(WIFI_TIMEOUT_MS)
                           ? remote_live(&g_transport, g_live_json, g_live_len, g_live_every)
                           : remote_live(&offline, g_live_json, g_live_len, g_live_every);
            static int last_status;
            if (r.status != last_status) {
                last_status = r.status;
                ESP_LOGI(TAG, "live: %d%s", r.status, r.status == 204 ? "" : " (keeps trying)");
            }
        } else {
            if (g_json_len == 0) {
                g_rs.req[0] = '\0';   /* nothing to send; the request is spent */
                r.status = -1;
            } else {
                r.status = net_up(WIFI_TIMEOUT_MS)
                               ? remote_upload(&g_rs, &g_transport, g_json, g_json_len, now_s())
                               : remote_upload(&g_rs, &offline, g_json, g_json_len, now_s());
            }
            ESP_LOGI(TAG, "upload: %d (%u bytes)", r.status, (unsigned)g_json_len);
        }
        static bool stack_logged;
        if (!stack_logged) {
            stack_logged = true;
            ESP_LOGI(TAG, "net task stack: %u bytes never used",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        xQueueSend(g_results, &r, portMAX_DELAY);
    }
}

static unsigned g_jobs_out;   /* main loop only */
static uint32_t g_job_since;  /* main loop only: when the outstanding job was posted */

static void post_job(job_kind_t job)
{
    /* Only ever called with nothing outstanding, so the queue has room and
     * this cannot block the CAN loop. */
    if (xQueueSend(g_jobs, &job, 0) == pdTRUE) {
        g_jobs_out++;
        g_job_since = now_ms();
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

/* ---- the panel, brought up only when it has to change -------------------- */

static bool g_display_ready;

/*
 * A timer wake with nothing to report never touches the panel: its pins are
 * still held from the last sleep and the "Zzz" stays on the glass. Only a
 * live screen (or a fresh "Zzz" over one) needs LVGL.
 */
static bool ensure_display(void)
{
    if (g_display_ready) {
        return true;
    }
    board_release_holds();
    if (board_display_init() == NULL) {
        return false;
    }
    ui_init();
    if (!board_lvgl_start()) {
        return false;
    }
    g_display_ready = true;
    return true;
}

/* Put "Zzz" on the glass and wait until it is really there. */
static void show_standby(void)
{
    if (!g_display_ready || !board_lvgl_lock(1000)) {
        return;
    }
    ui_standby(true);
    lv_refr_now(NULL);
    board_lvgl_unlock();
}

/* ---- the bus ------------------------------------------------------------- */

typedef struct {
    system_model_t model;
    poller_t       poller;
} bus_t;

/* The bus has been quiet: what the model holds is old. Start over so neither
 * the screen nor a report nor the virtual BMS repeats it. */
static void bus_restart(bus_t *b)
{
    bms_model_init(&b->model);
    poller_init(&b->poller, &b->model, poller_default_cfg());
}

void run_remote(void)
{
    static bus_t bus;
    bus_restart(&bus);

    settings_load(&g_settings);
    const remote_wake_t wake = board_wake_cause();
    /* RTC memory survives a reset that is not a power-on. Keep the state then,
     * so a crash loop cannot wipe the day's report count. */
    bool fresh = !remote_state_valid(&g_rs) || esp_reset_reason() == ESP_RST_POWERON;
    /* The ring is only worth keeping if the clock it was written with is
     * still running; a reset that restarted it shows as times in the future. */
    for (uint8_t i = 0; !fresh && i < g_rs.report_count; i++) {
        const uint8_t slot = (uint8_t)((g_rs.report_head + REMOTE_REPORTS_PER_DAY - 1 - i) %
                                       REMOTE_REPORTS_PER_DAY);
        fresh = g_rs.report_at_s[slot] > now_s() + 1;
    }
    if (fresh) {
        remote_state_init(&g_rs, g_settings.min_checkin_s);
    } else {
        g_rs.min_interval_s = g_settings.min_checkin_s ? g_settings.min_checkin_s : 1;
    }
    remote_on_wake(&g_rs, wake, now_s());
    if (g_hung == HUNG_MAGIC && esp_reset_reason() != ESP_RST_POWERON) {
        /* Last boot ended with a request that never finished: count it as a
         * failed check-in, so the board backs off rather than retrying at
         * once and hanging again. */
        const remote_transport_t offline = { no_network, NULL };
        remote_checkin(&g_rs, &offline, NULL, now_s(), esp_random(), NULL);
        ESP_LOGW(TAG, "last network request hung; backing off");
    }
    g_hung = 0;
    remote_wake_t last_wake = wake;
    ESP_LOGI(TAG, "woke: %s", wake_name(wake));

    const bool remote_ok = settings_remote_ready(&g_settings);
    if (remote_ok) {
        net_init(&g_settings);
        net_transport(&g_transport);
        console_set_network(true);
        g_jobs    = xQueueCreate(2, sizeof(job_kind_t));
        g_results = xQueueCreate(2, sizeof(result_t));
        xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK, NULL,
                                NET_TASK_PRIO, NULL, 0);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi or token set: display only (console: press the button within 10 s of a reset)");
    }

    /* Bring the bus up from the start. The transceiver stays in normal mode
     * while awake - in standby it could not drive the ACK bit for frames we
     * receive, and the controller would count every one as an error. Only
     * deep sleep puts it in standby. */
    board_can_standby(false);
    if (!twai_link_start()) {
        ESP_LOGE(TAG, "CAN did not come up; check the transceiver wiring");
    }
    vbms_t vbms;
    vbms_init(&vbms, VBMS_ENABLED);
    /* The virtual BMS's latest frames, until the poller's next request. */
    can_frame_out_t bcast[VBMS_MAX_FRAMES];
    uint8_t n_bcast = 0;

    pwr_cfg_t cfg = pwr_default_cfg();
    cfg.local_timeout_ms = (uint32_t)CONFIG_BMS_LOCAL_TIMEOUT_S * 1000u;
    pwr_t pwr;
    pwr_init(&pwr, cfg, wake, now_ms());

    bool polling = false;
    bool wake_pending = false;        /* a local wake to hand to remote.c */
    bool upload_wanted = false;       /* waiting for a running check-in */
    const bool console_gate = wake == REMOTE_WAKE_BOOT;
    bool input_pending = false, input_long = false;
    char report_req[REMOTE_REQ_LEN + 1] = "";
    uint32_t last_ui = 0, last_health = 0;
    pwr_state_t last_state = pwr.state;
    uint32_t local_since = now_ms(), last_live = 0;
    bool live_started = false;

    for (;;) {
        const uint32_t now = now_ms();

        /* ---- inputs ---- */
        bool long_press;
        if (board_key_poll(&long_press)) {
            const bool screen_was_live = pwr.state == PWR_LOCAL;
            /* A press soon after power-on or reset opens the console. */
            if (console_gate && now < CONSOLE_GATE_MS && !console_is_open()) {
                console_start(&g_settings);
            }
            if (pwr.state == PWR_IDLE) {
                bus_restart(&bus);
                wake_pending = true;     /* a wake, not every tap, checks in */
                last_wake = REMOTE_WAKE_TOUCH;
            }
            pwr_on_touch(&pwr, now);
            /* The first press only wakes the screen; it does not turn a page
             * nobody could see. */
            if (screen_was_live) {
                input_pending = true;
                input_long    = long_press;
            }
        }

        uint32_t id;
        uint8_t data[8], len;
        while (twai_link_recv(&id, data, &len)) {
            if (polling) {
                poller_on_frame(&bus.poller, id, data, len, now);
            }
        }

        /* ---- the network ---- */
        if (remote_ok) {
            result_t r;
            while (xQueueReceive(g_results, &r, 0) == pdTRUE) {
                g_jobs_out--;
                if (r.kind == JOB_CHECKIN && r.cmd == REMOTE_CMD_REPORT) {
                    if (pwr.state == PWR_IDLE) {
                        bus_restart(&bus);
                    }
                    memcpy(report_req, r.req, sizeof report_req);
                    pwr_on_report_cmd(&pwr, now);
                } else if (r.kind == JOB_UPLOAD) {
                    pwr_on_upload_done(&pwr, now);
                }
            }

            if (g_jobs_out == 0 && upload_wanted) {
                post_job(JOB_UPLOAD);
                if (g_jobs_out == 0) {
                    pwr_on_upload_done(&pwr, now);   /* could not queue: give up */
                }
                upload_wanted = false;
            }
            /* While the screen is live, a snapshot every few seconds - after
             * any check-in, which keeps its place. */
            const uint32_t live_s = remote_live_interval(&g_rs, g_settings.live_s);
            if (g_jobs_out == 0 && !upload_wanted && pwr.state == PWR_LOCAL && live_s > 0 &&
                !remote_checkin_due(&g_rs, now_s()) &&
                (pwr_snapshot_complete(&bus.model) || now - local_since >= LIVE_FIRST_MS) &&
                (!live_started || now - last_live >= live_s * 1000u)) {
                warning_set_t w;
                warnings_evaluate(&bus.model, &w);
                const snapshot_meta_t meta = { NULL, BMS_FW_VERSION, now_s() };
                g_live_len = snapshot_json_encode(&bus.model, &w, &meta, g_live_json,
                                                  sizeof g_live_json);
                g_live_every = live_s;
                if (g_live_len) {
                    post_job(JOB_LIVE);
                }
                last_live = now;
                live_started = true;
            }
            if (g_jobs_out == 0) {
                if (console_take_checkin_request()) {
                    g_rs.checkin_due_now = true;
                }
                if (wake_pending) {
                    remote_on_wake(&g_rs, REMOTE_WAKE_TOUCH, now_s());
                    wake_pending = false;
                }
                if (remote_checkin_due(&g_rs, now_s())) {
                    snprintf(g_query, sizeof g_query, "fw=%s&state=%s&wake=%s",
                             BMS_FW_VERSION,
                             pwr.state == PWR_LOCAL ? "local" : "standby",
                             wake_name(last_wake));
                    post_job(JOB_CHECKIN);
                    last_wake = REMOTE_WAKE_TIMER;
                }
            }
        }

        /* ---- what power.c wants ---- */
        if (pwr_bank_active(&bus.model)) {
            pwr_on_bank_active(&pwr, now);   /* current flows: keep the screen on */
        }
        const pwr_out_t out = pwr_tick(&pwr, now, pwr_snapshot_complete(&bus.model));
        if (pwr.state != last_state) {
            if (pwr.state == PWR_LOCAL) {
                local_since  = now;
                live_started = false;   /* the first one once the data is in */
            }
            last_state = pwr.state;
        }

        if (out.poll_can != polling) {
            polling = out.poll_can;
            n_bcast = 0;   /* nothing from before a restart goes out after it */
            if (!polling) {
                bus_restart(&bus);
            }
        }
        if (polling) {
            /* Only while polling: in IDLE the model has been cleared, and the
             * controller should time the battery out, not read stale data. */
            can_frame_out_t fresh[VBMS_MAX_FRAMES];
            const uint8_t nb = vbms_tick(&vbms, &bus.model, now, fresh);
            if (nb) {
                memcpy(bcast, fresh, sizeof fresh);
                n_bcast = nb;
            }
            can_frame_out_t tx;
            if (poller_tick(&bus.poller, now, &tx)) {
                /* Held for the next request, as in main.c: the last reply has
                 * had its gap, so a separate bus can borrow the controller. */
                if (n_bcast) {
                    twai_link_send_victron(bcast, n_bcast);
                    n_bcast = 0;
                }
                twai_link_send(tx.id, tx.ext, tx.data, tx.len);
            }
        }

        if (out.upload_now && remote_ok) {
            warning_set_t w;
            warnings_evaluate(&bus.model, &w);
            const snapshot_meta_t meta = { report_req, BMS_FW_VERSION, now_s() };
            /* Encoded now, while the data is complete; sent as soon as no
             * check-in is running (the network task may be reading g_query,
             * never g_json, so writing it here is safe). */
            g_json_len = snapshot_json_encode(&bus.model, &w, &meta, g_json, sizeof g_json);
            upload_wanted = true;
        } else if (out.upload_now) {
            pwr_on_upload_done(&pwr, now);
        }

        /* ---- the screen ---- */
        if (out.screen_live && ensure_display()) {
            if (board_lvgl_lock(20)) {
                if (ui_in_standby()) {
                    ui_standby(false);
                    ui_update(&bus.model);
                }
                if (input_pending) {
                    ui_input(input_long ? UI_KEY_LONG : UI_KEY_SHORT);
                    ui_update(&bus.model);
                    input_pending = false;
                }
                if (now - last_ui >= UI_UPDATE_MS) {
                    last_ui = now;
                    int rssi;
                    if (!remote_ok || !net_tried()) {
                        ui_set_wifi(UI_WIFI_NONE, 0);
                    } else if (net_rssi(&rssi)) {
                        ui_set_wifi(UI_WIFI_UP, ui_wifi_bars(rssi));
                    } else {
                        ui_set_wifi(UI_WIFI_DOWN, 0);
                    }
                    ui_update(&bus.model);
                }
                board_lvgl_unlock();
            }
        } else if (!out.screen_live && g_display_ready && !ui_in_standby()) {
            show_standby();
        }

        /* ---- sleep ---- */
        if (out.sleep_now && g_jobs_out == 0 && !upload_wanted && !wake_pending &&
            !console_is_open() && (!remote_ok || !remote_checkin_due(&g_rs, now_s()))) {
            const uint32_t wait = remote_ok ? remote_seconds_until_checkin(&g_rs, now_s())
                                            : UINT32_MAX;
            if (remote_wait_is_deep(wait)) {
                show_standby();
                if (remote_ok) {
                    net_down();
                }
                twai_link_stop();
                board_deep_sleep(wait);   /* does not return */
            }
            /* A short interval is waited out awake: Wi-Fi stays associated,
             * the bus stays quiet. */
        }

        /* A fresh clock: a job posted earlier in this pass is stamped after
         * `now`, and `now - g_job_since` would wrap to ~49 days. */
        if (g_jobs_out > 0 && now_ms() - g_job_since > JOB_WATCHDOG_MS) {
            ESP_LOGE(TAG, "network request still running after %d s; restarting",
                     JOB_WATCHDOG_MS / 1000);
            g_hung = HUNG_MAGIC;
            esp_restart();
        }

        /* ---- bus health, as in the always-on loop ---- */
        if (now - last_health >= 2000) {
            last_health = now;
            uint32_t tx_failed, rx_missed, bus_errors;
            bool needs_recovery;
            twai_link_stats(&tx_failed, &rx_missed, &bus_errors, &needs_recovery);
            if (needs_recovery) {
                twai_link_recover();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(out.sleep_now ? 20 : 2));
    }
}

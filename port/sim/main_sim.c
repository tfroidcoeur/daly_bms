/*
 * Host GUI simulator.
 *
 * Runs the real UI against the real poller and decoder, over a real CAN socket.
 * The only thing that is not the device is the display backend and the clock.
 *
 *     ./build/sim [interface]        (default vcan0)
 *
 * Keys:  n = KEY short press (next page)
 *        N = KEY long press  (cell detail)
 *        q / Esc = quit
 *
 * For unattended capture (screenshots, regression checks):
 *   SIM_SCRIPT="3000:n,1500:n,1500:N"   time_ms:key pairs, applied in order
 *   SIM_SHOT_DIR=shots                  write a PPM after each scripted step
 *   SIM_QUIT_MS=12000                   exit after this long
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bms_model.h"
#include "can_socketcan.h"
#include "lvgl.h"
#include "poller.h"
#include "sdl_display.h"
#include "ui.h"

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t tick_cb(void) { return host_millis(); }

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "vcan0";

    can_link_t link;
    if (!can_link_open(&link, ifname, true)) {
        return 1;
    }
    signal(SIGINT, on_sigint);

    lv_init();
    lv_tick_set_cb(tick_cb);
    if (sim_display_init() == NULL) {
        return 1;
    }
    /*
     * Specimen mode replaces the whole UI, so the normal pages do not exist and
     * ui_update() must not be called against them. LVGL's default assert
     * handler is `while(1)`, so touching a NULL widget hangs rather than
     * crashing - which is a confusing way to find this out.
     */
    const bool specimen = getenv("SIM_FONT_SPECIMEN") != NULL;
    if (specimen) {
        void ui_font_specimen(void);
        ui_font_specimen();
    } else {
        ui_init();
    }

    system_model_t model;
    poller_t poller;
    bms_model_init(&model);
    poller_init(&poller, &model, poller_default_cfg());

    printf("sim: %s   keys: n = next page, N = cell detail, q = quit\n", ifname);

    /* Optional unattended script - see the header comment. */
    char script[256] = { 0 };
    const char *env = getenv("SIM_SCRIPT");
    if (env) {
        snprintf(script, sizeof script, "%s", env);
    }
    const char *shot_dir = getenv("SIM_SHOT_DIR");
    const uint32_t quit_ms =
        getenv("SIM_QUIT_MS") ? (uint32_t)atoi(getenv("SIM_QUIT_MS")) : 0;
    char *script_pos = script[0] ? script : NULL;
    uint32_t next_step_at = 0;
    int step_no = 0;
    bool step_pending = false;
    char step_key = 0;

    uint32_t last_ui = 0;

    while (!g_stop && sim_poll_events()) {
        const uint32_t now = host_millis();

        can_frame_out_t tx;
        if (poller_tick(&poller, now, &tx)) {
            can_link_send(&link, tx.id, tx.data, tx.len);
        }

        uint32_t id;
        uint8_t data[8], len;
        while (can_link_recv(&link, &id, data, &len)) {
            poller_on_frame(&poller, id, data, len, now);
        }

        /* The panel is reflective and slow. Pushing the model into the widgets
         * more than about twice a second buys nothing and smears the display. */
        if (!specimen && now - last_ui >= 500) {
            last_ui = now;
            ui_update(&model);
        }

        /* Scripted input: parse one "delay:key" pair at a time. */
        if (script_pos && !step_pending) {
            int delay = 0;
            char key = 0;
            if (sscanf(script_pos, "%d:%c", &delay, &key) == 2) {
                next_step_at = now + (uint32_t)delay;
                step_key = key;
                step_pending = true;
                char *comma = strchr(script_pos, ',');
                script_pos = comma ? comma + 1 : NULL;
            } else {
                script_pos = NULL;
            }
        }
        if (step_pending && now >= next_step_at) {
            if (!specimen) {
                ui_update(&model);
            }
            lv_timer_handler();
            if (shot_dir) {
                char path[320];
                snprintf(path, sizeof path, "%s/%02d-%s.ppm", shot_dir,
                         step_no, ui_cells_open() ? "cells" : "page");
                sim_screenshot(path);
                printf("shot %s (page %d, cells %d)\n", path,
                       (int)ui_current_page(), (int)ui_cells_open());
            }
            step_no++;
            ui_input(step_key == 'N' ? UI_KEY_LONG : UI_KEY_SHORT);
            step_pending = false;
        }
        if (quit_ms && now >= quit_ms) {
            if (shot_dir) {
                char path[320];
                snprintf(path, sizeof path, "%s/%02d-final.ppm", shot_dir, step_no);
                if (!specimen) {
                    ui_update(&model);
                }
                lv_timer_handler();
                sim_screenshot(path);
                printf("shot %s\n", path);
            }
            break;
        }

        lv_timer_handler();
        sleep_ms(5);
    }

    can_link_close(&link);
    sim_deinit();
    return 0;
}

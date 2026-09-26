/*
 * Daly BMS monitor - ESP32-S3-RLCD-4.2 firmware.
 *
 * Polls three Daly BMS units over one shared CAN bus and draws them on the
 * 300 x 400 reflective panel. core/ and ui/ are the same sources the host
 * simulator builds; only this file, board.c and twai_link.c are device-specific.
 *
 * Three build modes, chosen under "Daly BMS monitor" in idf.py menuconfig:
 *
 *   normal     - poll and display.
 *   logger     - dump every transmitted and received frame over USB serial and
 *                draw nothing. Use this FIRST on real hardware to validate the
 *                field layouts in docs/hardware/daly-can-protocol.md before
 *                trusting the display.
 *   loop test  - bit-bang the transceiver with TWAI switched off, to find out
 *                why the bus is silent. See can_loop_test() below.
 */
#include <inttypes.h>
#include <stdio.h>

#include "board.h"
#include "bms_model.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "poller.h"
#include "twai_link.h"
#include "ui.h"

static const char *TAG = "bms";

/* The reflective panel is slow; twice a second is as fast as it is worth
 * pushing new values into the widgets. */
#define UI_UPDATE_MS 500

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#ifdef CONFIG_BMS_RAW_LOGGER

/*
 * Bring-up mode: ask each pack for everything and print exactly what comes
 * back, so the decode table can be checked against Daly's own app.
 */
static void raw_logger(void)
{
    system_model_t model;
    poller_t poller;

    bms_model_init(&model);
    poller_init(&poller, &model, poller_default_cfg());

    ESP_LOGI(TAG, "raw frame logger - validate before trusting the UI");

    while (true) {
        const uint32_t now = now_ms();

        can_frame_out_t tx;
        if (poller_tick(&poller, now, &tx)) {
            twai_link_send(tx.id, tx.data, tx.len);
            printf("TX %08" PRIX32 "\n", tx.id);
        }

        uint32_t id;
        uint8_t data[8], len;
        while (twai_link_recv(&id, data, &len)) {
            /* Only the bytes the frame actually carried - this dump is what the
             * decode table gets checked against, so padding would be a lie. */
            printf("RX %08" PRIX32 " [%u]", id, len);
            for (uint8_t i = 0; i < len; i++) {
                printf(" %02X", data[i]);
            }
            printf("\n");
            poller_on_frame(&poller, id, data, len, now);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

#endif /* CONFIG_BMS_RAW_LOGGER */

#ifdef CONFIG_BMS_CAN_LOOP_TEST

/*
 * Why this exists.
 *
 * Every board-side CAN fault looks the same through TWAI. A transceiver with
 * no 3V3, a transceiver whose Rs is left floating (so its driver never
 * enables), CANH/CANL not reaching the bus, and TX/RX wired to each other's
 * pins ALL produce the same symptom: the controller transmits, cannot read its
 * own bits back, and reports bus errors at roughly the bit rate. The error
 * code capture register would tell them apart, but ESP-IDF does not surface it.
 *
 * So take TWAI out of the picture and walk a level around the loop by hand:
 *
 *     TX pin -> transceiver D -> CANH/CANL -> transceiver R -> RX pin
 *
 * A pass means the entire board-side chain is good and the fault is further
 * out - the cable, or the far node. A fail says which half is broken.
 */

static void pin_out(int pin)
{
    const gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&c));
}

static void pin_in(int pin, gpio_pullup_t up, gpio_pulldown_t down)
{
    const gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = up,
        .pull_down_en = down,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&c));
}

/* True when `in` follows `out` in both directions, i.e. the loop is closed. */
static bool loop_follows(int out, int in)
{
    pin_out(out);
    pin_in(in, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);

    gpio_set_level(out, 0);                 /* dominant */
    esp_rom_delay_us(200);
    const int saw_low = gpio_get_level(in);

    gpio_set_level(out, 1);                 /* recessive */
    esp_rom_delay_us(200);
    const int saw_high = gpio_get_level(in);

    /* Leave the bus released rather than held dominant. */
    pin_in(out, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);

    ESP_LOGI(TAG, "  GPIO%d low  -> GPIO%d reads %d   (want 0)", out, in, saw_low);
    ESP_LOGI(TAG, "  GPIO%d high -> GPIO%d reads %d   (want 1)", out, in, saw_high);
    return saw_low == 0 && saw_high == 1;
}

/*
 * Is anything outside the chip holding this pin, or does it just follow
 * whichever internal pull we enable? A pin that tracks the pull is connected
 * to nothing that drives.
 */
static bool externally_driven(int pin)
{
    pin_in(pin, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    esp_rom_delay_us(500);
    const int with_pullup = gpio_get_level(pin);

    pin_in(pin, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE);
    esp_rom_delay_us(500);
    const int with_pulldown = gpio_get_level(pin);

    pin_in(pin, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);

    const bool driven = with_pullup == with_pulldown;
    ESP_LOGI(TAG, "  GPIO%d: pull-up reads %d, pull-down reads %d  -> %s",
             pin, with_pullup, with_pulldown, driven ? "driven" : "FLOATING");
    return driven;
}

static void can_loop_test(void)
{
    ESP_LOGI(TAG, "CAN transceiver loop test - TWAI is not started at all.");
    ESP_LOGI(TAG, "Expecting GPIO%d -> transceiver CTX/D, GPIO%d <- CRX/R.",
             BOARD_CAN_TX, BOARD_CAN_RX);

    while (true) {
        ESP_LOGI(TAG, "--------");

        const bool rx_driven = externally_driven(BOARD_CAN_RX);

        ESP_LOGI(TAG, "forward, TX GPIO%d -> RX GPIO%d:",
                 BOARD_CAN_TX, BOARD_CAN_RX);
        const bool forward_ok = loop_follows(BOARD_CAN_TX, BOARD_CAN_RX);
        if (forward_ok) {
            ESP_LOGI(TAG, "PASS: the loop is closed and the right way round.");
            ESP_LOGI(TAG, "      Transceiver powered, Rs grounded, bus wired.");
            ESP_LOGI(TAG, "      A silent bus now means the cable or the far node.");
        } else if (!rx_driven) {
            ESP_LOGE(TAG, "FAIL: GPIO%d floats - nothing is driving it.",
                     BOARD_CAN_RX);
            ESP_LOGE(TAG, "      Check, in this order: 3V3 on the transceiver,");
            ESP_LOGE(TAG, "      Rs tied to GND, then CRX/R reaching GPIO%d.",
                     BOARD_CAN_RX);
        } else {
            /*
             * Driven but unmoving. Which level it is stuck at says which half
             * of the transceiver is unhappy, and they need different fixes.
             */
            pin_in(BOARD_CAN_RX, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
            const int stuck = gpio_get_level(BOARD_CAN_RX);

            ESP_LOGE(TAG, "FAIL: GPIO%d is stuck %s and does not follow GPIO%d.",
                     BOARD_CAN_RX, stuck ? "HIGH" : "LOW", BOARD_CAN_TX);
            if (stuck == 0) {
                ESP_LOGE(TAG, "      Stuck low = the receiver sees the bus as");
                ESP_LOGE(TAG, "      permanently DOMINANT. Something holds CANH");
                ESP_LOGE(TAG, "      above CANL - most often CANL shorted to GND");
                ESP_LOGE(TAG, "      (RJ45 pins 2 and 3 are adjacent), or CANH");
                ESP_LOGE(TAG, "      shorted to 3V3. Measure CANL-to-GND: it");
                ESP_LOGE(TAG, "      should be kilohms, not zero.");
            } else {
                ESP_LOGE(TAG, "      Stuck high = the receiver sees the bus as");
                ESP_LOGE(TAG, "      permanently recessive, so the driver is not");
                ESP_LOGE(TAG, "      reaching it: check Rs to GND, then that");
                ESP_LOGE(TAG, "      CANH/CANL leave the transceiver at all.");
            }
        }

        /*
         * Whatever the forward direction did, if it failed, try it reversed.
         * A swap makes our TX drive the transceiver's R output, so the RX pin
         * reads as "driven" rather than floating - which means a swap can look
         * like any of the failures above and has to be tested for explicitly.
         *
         * Safe only because we are here at all: the forward test has already
         * failed, so the pins are not wired the way we expect.
         */
        if (!forward_ok) {
            ESP_LOGI(TAG, "retrying reversed, in case CTX/CRX are swapped:");
            if (loop_follows(BOARD_CAN_RX, BOARD_CAN_TX)) {
                ESP_LOGE(TAG, "SWAPPED: the loop closes with the pins reversed.");
                ESP_LOGE(TAG, "         Move CTX/D to GPIO%d and CRX/R to GPIO%d.",
                         BOARD_CAN_TX, BOARD_CAN_RX);
            } else {
                ESP_LOGI(TAG, "  not a swap - reversed does not work either.");
            }
        }

        /*
         * Then hold each bus level long enough to meter. Resistance readings
         * only prove a path exists inside your own cable; this puts a real
         * differential on the wire, so it can be followed to any point in it -
         * including the far plug, unplugged from the adapter.
         */
        ESP_LOGW(TAG, "holding DOMINANT for 6 s - expect CANH ~3.5 V, CANL ~1.5 V");
        pin_out(BOARD_CAN_TX);
        gpio_set_level(BOARD_CAN_TX, 0);
        vTaskDelay(pdMS_TO_TICKS(6000));

        ESP_LOGW(TAG, "holding RECESSIVE for 6 s - expect both ~2.5 V");
        gpio_set_level(BOARD_CAN_TX, 1);
        vTaskDelay(pdMS_TO_TICKS(6000));

        pin_in(BOARD_CAN_TX, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif /* CONFIG_BMS_CAN_LOOP_TEST */

void app_main(void)
{
    board_key_init();

#ifdef CONFIG_BMS_TOUCH_MONITOR
    /*
     * Nothing but the pads. board_key_init() has already brought the touch
     * controller up and logged the benchmarks; this just keeps reporting so a
     * finger's effect is visible rather than inferred.
     */
    ESP_LOGI(TAG, "touch monitor - touch a pad and watch the delta");
    while (true) {
        ESP_LOGI(TAG, "--------");
        board_touch_report();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
#endif

#ifdef CONFIG_BMS_CAN_LOOP_TEST
    can_loop_test();   /* never returns */
    return;
#endif

    if (!twai_link_start()) {
        ESP_LOGE(TAG, "CAN did not come up; check the transceiver wiring");
    }

#ifdef CONFIG_BMS_RAW_LOGGER
    raw_logger();
    return;
#else
    lv_display_t *disp = board_display_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    /*
     * Build the UI before the LVGL task exists, so no lock is needed yet, then
     * hand LVGL over to its own task. From here on every ui_*() call must be
     * inside board_lvgl_lock() / board_lvgl_unlock().
     */
    ui_init();

    if (!board_lvgl_start()) {
        ESP_LOGE(TAG, "LVGL task did not start");
        return;
    }

    system_model_t model;
    poller_t poller;
    bms_model_init(&model);
    poller_init(&poller, &model, poller_default_cfg());

    uint32_t last_ui = 0;
    uint32_t last_health = 0;

    /*
     * A key press is latched rather than applied immediately. Taking the LVGL
     * lock can fail while the LVGL task is mid-repaint, and a dropped press
     * would read as a dead button - the one thing the single control on this
     * board must never do. The latch retries every pass until it lands.
     */
    bool input_pending = false;
    bool input_long    = false;

    while (true) {
        const uint32_t now = now_ms();

        can_frame_out_t tx;
        if (poller_tick(&poller, now, &tx)) {
            twai_link_send(tx.id, tx.data, tx.len);
        }

        uint32_t id;
        uint8_t data[8], len;
        while (twai_link_recv(&id, data, &len)) {
            poller_on_frame(&poller, id, data, len, now);
        }

        /*
         * The UI is touched only under the lock, and never with an indefinite
         * wait - blocking here would stall the CAN poll loop and make packs
         * look offline. Whatever cannot be done this pass is retried next pass,
         * a couple of milliseconds later.
         */
        bool long_press;
        if (board_key_poll(&long_press)) {
            input_pending = true;
            input_long    = long_press;
        }

        if (input_pending && board_lvgl_lock(20)) {
            ui_input(input_long ? UI_KEY_LONG : UI_KEY_SHORT);
            ui_update(&model);
            board_lvgl_unlock();
            input_pending = false;
        }

        /* last_ui only advances once the update actually happened. */
        if (now - last_ui >= UI_UPDATE_MS && board_lvgl_lock(20)) {
            last_ui = now;
            ui_update(&model);
            board_lvgl_unlock();
        }

        /*
         * A bus fault leaves the controller silent until it is recovered, and
         * recovery is two steps: bus-off -> stopped -> running. Calling
         * twai_link_recover() on each 2 s tick walks it through both.
         */
        if (now - last_health >= 2000) {
            last_health = now;
            uint32_t tx_failed, rx_missed, bus_errors;
            bool needs_recovery;
            twai_link_stats(&tx_failed, &rx_missed, &bus_errors,
                            &needs_recovery);
            if (needs_recovery) {
                twai_link_recover();
            } else if (bus_errors) {
                ESP_LOGW(TAG, "bus errors %" PRIu32 " tx_failed %" PRIu32
                              " rx_missed %" PRIu32,
                         bus_errors, tx_failed, rx_missed);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
#endif
}

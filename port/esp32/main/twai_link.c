#include "twai_link.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "twai";

/*
 * Install and start the driver on one transceiver. Pins, timing and filter are
 * all fixed at install time, which is why moving to the second bus is a full
 * uninstall and reinstall.
 */
static bool install(gpio_num_t tx, gpio_num_t rx, uint32_t rx_queue_len,
                    const twai_timing_config_t *t, const twai_filter_config_t *f)
{
    const twai_general_config_t g = {
        .mode            = TWAI_MODE_NORMAL,
        .tx_io           = tx,
        .rx_io           = rx,
        .clkout_io       = TWAI_IO_UNUSED,
        .bus_off_io      = TWAI_IO_UNUSED,
        .tx_queue_len    = 8,
        .rx_queue_len    = rx_queue_len,
        .alerts_enabled  = TWAI_ALERT_ALL,
        .clkout_divider  = 0,
        .intr_flags      = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t err = twai_driver_install(&g, t, f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_start: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return false;
    }
    return true;
}

/*
 * Accept only BMS responses: priority 0x18 (bits 24-28) and destination
 * 0x40 (bits 8-15), with the command byte and source address left free.
 *
 * The TWAI acceptance filter compares against the identifier left-aligned
 * in a 32-bit word: for extended frames the 29-bit ID sits in bits 31..3.
 */
static twai_filter_config_t daly_filter(void)
{
    const uint32_t id_bits   = (0x18u << 24) | (0x40u << 8);
    const uint32_t care_bits = 0x1F00FF00u;
    const twai_filter_config_t f = {
        .acceptance_code = id_bits << 3,
        /* The TWAI mask is inverted: a 1 bit means "do not care". */
        .acceptance_mask = ~(care_bits << 3),
        .single_filter   = true,
    };
    return f;
}

static bool start_packs(void)
{
    const twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
    const twai_filter_config_t f = daly_filter();
    /* rx queue: a 0x95 burst is 6 frames back to back */
    return install(BOARD_CAN_TX, BOARD_CAN_RX, 32, &t, &f);
}

bool twai_link_start(void)
{
    if (!start_packs()) {
        return false;
    }
    ESP_LOGI(TAG, "up at 250 kbit/s on TX=%d RX=%d", BOARD_CAN_TX, BOARD_CAN_RX);
    return true;
}

void twai_link_stop(void)
{
    twai_stop();   /* fails in bus-off, where uninstall is allowed anyway */
    twai_driver_uninstall();
}

#ifdef CONFIG_BMS_VICTRON_SEPARATE_BUS

/* Two frames take about 0.5 ms at 500 kbit/s; this only ends a slot nobody ACKs. */
#define VICTRON_SLOT_US  5000
/* An unanswered slot is logged once per this many. */
#define VICTRON_WARN_EVERY 60

/*
 * Uninstalling the driver only disables the TX pin's output, and a
 * transceiver's D left floating can read as dominant. The bus not in use is
 * held recessive by plain GPIO until the driver claims the pin again.
 */
static void hold_recessive(gpio_num_t pin)
{
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

bool twai_link_send_victron(const can_frame_out_t *f, uint8_t n)
{
#ifdef CONFIG_BMS_VICTRON_BITRATE_250K
    const twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
#else
    const twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
#endif
    /* Send-only. Every bit must match an all-ones identifier, which only a
     * remote frame with ID 0x7FF or 0x1FFFFFFF could; nothing here sends one. */
    const twai_filter_config_t none = {
        .acceptance_code = 0xFFFFFFFFu,
        .acceptance_mask = 0,
        .single_filter   = true,
    };
    static uint32_t unanswered;

    twai_link_stop();
    hold_recessive(BOARD_CAN_TX);
    bool ok = install(BOARD_VICTRON_TX, BOARD_VICTRON_RX, 1, &t, &none);
    if (ok) {
        for (uint8_t i = 0; i < n; i++) {
            ok = twai_link_send(f[i].id, f[i].ext, f[i].data, f[i].len) && ok;
        }
        /*
         * msgs_to_tx counts a frame until it completes. Nobody on the bus to
         * ACK it - the controller off, the cable out - and the controller
         * retransmits forever, so the wait is bounded; stopping the driver
         * below drops whatever is left.
         */
        const int64_t until = esp_timer_get_time() + VICTRON_SLOT_US;
        twai_status_info_t s;
        while (twai_get_status_info(&s) == ESP_OK && s.msgs_to_tx > 0) {
            if (esp_timer_get_time() >= until) {
                ok = false;
                break;
            }
            esp_rom_delay_us(50);
        }
        twai_link_stop();
    }
    hold_recessive(BOARD_VICTRON_TX);
    if (!ok && unanswered++ % VICTRON_WARN_EVERY == 0) {
        ESP_LOGW(TAG, "victron bus: frames not acknowledged (%" PRIu32 " slots)",
                 unanswered);
    }

    /* Back to the packs whatever happened: they are what the screen shows. */
    if (!start_packs()) {
        ESP_LOGE(TAG, "could not return to the pack bus");
    }
    return ok;
}

#else

bool twai_link_send_victron(const can_frame_out_t *f, uint8_t n)
{
    bool ok = true;
    for (uint8_t i = 0; i < n; i++) {
        ok = twai_link_send(f[i].id, f[i].ext, f[i].data, f[i].len) && ok;
    }
    return ok;
}

#endif /* CONFIG_BMS_VICTRON_SEPARATE_BUS */

bool twai_link_send(uint32_t id, bool ext, const uint8_t *data, uint8_t len)
{
    twai_message_t m;
    memset(&m, 0, sizeof m);
    m.identifier       = id & (ext ? 0x1FFFFFFFu : 0x7FFu);
    m.extd             = ext;
    m.data_length_code = len > 8 ? 8 : len;
    memcpy(m.data, data, m.data_length_code);

    /* Never block the main loop on a stalled bus. */
    return twai_transmit(&m, 0) == ESP_OK;
}

bool twai_link_recv(uint32_t *ext_id, uint8_t *data, uint8_t *len)
{
    /*
     * Callers drain with `while (twai_link_recv(...))`, so a frame we do not
     * want must be skipped rather than returned as "queue empty" - otherwise
     * one stray standard-ID frame ends the pass and strands the backlog.
     */
    for (;;) {
        twai_message_t m;
        memset(&m, 0, sizeof m);   /* twai_receive fills only DLC bytes */
        if (twai_receive(&m, 0) != ESP_OK) {
            return false;
        }
        if (!m.extd) {
            continue;
        }
        *ext_id = m.identifier;
        *len    = m.data_length_code > 8 ? 8 : m.data_length_code;
        memcpy(data, m.data, 8);
        return true;
    }
}

void twai_link_stats(uint32_t *tx_failed, uint32_t *rx_missed,
                     uint32_t *bus_errors, bool *needs_recovery)
{
    *tx_failed      = 0;
    *rx_missed      = 0;
    *bus_errors     = 0;
    *needs_recovery = false;

    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) {
        return;   /* driver not installed; the caller keeps its zeros */
    }
    *tx_failed  = s.tx_failed_count;
    *rx_missed  = s.rx_missed_count;
    *bus_errors = s.bus_error_count;
    /* STOPPED counts: that is where initiate_recovery() leaves the controller,
     * and nothing else in this app ever stops it. */
    *needs_recovery = s.state == TWAI_STATE_BUS_OFF ||
                      s.state == TWAI_STATE_STOPPED;
}

void twai_link_recover(void)
{
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) {
        return;
    }

    /*
     * Recovery is two steps and the first one is asynchronous.
     * twai_initiate_recovery() waits for 128 bus-free occurrences and then
     * leaves the controller STOPPED, not running - so without the twai_start()
     * below, a single transient bus fault silences CAN until a power cycle.
     */
    if (s.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGW(TAG, "bus-off; initiating recovery");
        if (twai_initiate_recovery() != ESP_OK) {
            ESP_LOGE(TAG, "twai_initiate_recovery failed");
        }
    } else if (s.state == TWAI_STATE_STOPPED) {
        if (twai_start() == ESP_OK) {
            ESP_LOGI(TAG, "bus recovered; running again");
        } else {
            ESP_LOGE(TAG, "twai_start after recovery failed");
        }
    }
}

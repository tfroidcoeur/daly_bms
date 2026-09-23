#include "twai_link.h"

#include <string.h>

#include "board.h"
#include "driver/twai.h"
#include "esp_log.h"

static const char *TAG = "twai";

bool twai_link_start(void)
{
    const twai_general_config_t g = {
        .mode            = TWAI_MODE_NORMAL,
        .tx_io           = BOARD_CAN_TX,
        .rx_io           = BOARD_CAN_RX,
        .clkout_io       = TWAI_IO_UNUSED,
        .bus_off_io      = TWAI_IO_UNUSED,
        .tx_queue_len    = 8,
        .rx_queue_len    = 32,   /* a 0x95 burst is 6 frames back to back */
        .alerts_enabled  = TWAI_ALERT_ALL,
        .clkout_divider  = 0,
        .intr_flags      = ESP_INTR_FLAG_LEVEL1,
    };
    const twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();

    /*
     * Accept only BMS responses: priority 0x18 (bits 24-28) and destination
     * 0x40 (bits 8-15), with the command byte and source address left free.
     *
     * The TWAI acceptance filter compares against the identifier left-aligned
     * in a 32-bit word: for extended frames the 29-bit ID sits in bits 31..3.
     */
    const uint32_t id_bits   = (0x18u << 24) | (0x40u << 8);
    const uint32_t care_bits = 0x1F00FF00u;
    const twai_filter_config_t f = {
        .acceptance_code = id_bits << 3,
        /* The TWAI mask is inverted: a 1 bit means "do not care". */
        .acceptance_mask = ~(care_bits << 3),
        .single_filter   = true,
    };

    esp_err_t err = twai_driver_install(&g, &t, &f);
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
    ESP_LOGI(TAG, "up at 250 kbit/s on TX=%d RX=%d", BOARD_CAN_TX, BOARD_CAN_RX);
    return true;
}

void twai_link_stop(void)
{
    twai_stop();
    twai_driver_uninstall();
}

bool twai_link_send(uint32_t ext_id, const uint8_t *data, uint8_t len)
{
    twai_message_t m;
    memset(&m, 0, sizeof m);
    m.identifier       = ext_id & 0x1FFFFFFFu;
    m.extd             = 1;
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

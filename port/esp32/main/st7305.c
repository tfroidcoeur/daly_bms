#include "st7305.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "st7305";

/* Commands used here. Full set in the ST7305 datasheet. */
#define CMD_SLPOUT       0x11
#define CMD_INVON        0x21
#define CMD_CASET        0x2A
#define CMD_RASET        0x2B
#define CMD_RAMWR        0x2C
#define CMD_TEON         0x35
#define CMD_MADCTL       0x36
#define CMD_HPM          0x38   /* high power mode */
#define CMD_LPM          0x39   /* low power mode  */
#define CMD_DISPON       0x29
#define CMD_DISPOFF      0x28
#define CMD_COLMOD       0x3A
#define CMD_DUTY         0xB0
#define CMD_FRAME_RATE   0xB2
#define CMD_GATE_HPM     0xB3
#define CMD_GATE_LPM     0xB4
#define CMD_SRC_SET      0xB7
#define CMD_PANEL_SET    0xB8
#define CMD_SRC_VOLT     0xB9
#define CMD_GATE_VOLT    0xC0
#define CMD_VSHP         0xC1
#define CMD_VSLP         0xC2
#define CMD_VSHN         0xC4
#define CMD_VSLN         0xC5
#define CMD_CLR_RAM      0xC9
#define CMD_BOOSTER      0xD1
#define CMD_NVM_LOAD     0xD6
#define CMD_OSC          0xD8
#define CMD_AUTOPWR      0xD0
#define CMD_GAMMA        0x62

/*
 * Window covering the full 300 x 400 panel.
 *
 * Each column address spans 12 pixels (3 bytes of 4), so 300 px = 25 columns,
 * 0x12..0x2A. Each page is a row pair, so 400 px = 200 pages, 0x00..0xC7.
 * The 0x12 start offset is this panel's placement within the controller's
 * larger addressable area - the Zephyr devicetree calls the same thing
 * `start-column`.
 */
#define WIN_COL_START  0x12
#define WIN_COL_END    0x2A
#define WIN_PAGE_START 0x00
#define WIN_PAGE_END   0xC7

struct st7305_t {
    esp_lcd_panel_io_handle_t io;
    int      rst_gpio;
    int      spi_host;   /* -1 until the bus is ours to free */
    uint8_t *fb;
};

static esp_err_t cmd(st7305_t *d, uint8_t c)
{
    return esp_lcd_panel_io_tx_param(d->io, c, NULL, 0);
}

static esp_err_t cmd_data(st7305_t *d, uint8_t c, const uint8_t *data, size_t n)
{
    return esp_lcd_panel_io_tx_param(d->io, c, data, n);
}

static void reset_panel(st7305_t *d)
{
    gpio_set_level(d->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(d->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(d->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t set_window(st7305_t *d)
{
    const uint8_t col[2]  = { WIN_COL_START, WIN_COL_END };
    const uint8_t page[2] = { WIN_PAGE_START, WIN_PAGE_END };

    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_CASET, col, 2), TAG, "caset");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_RASET, page, 2), TAG, "raset");
    return ESP_OK;
}

static esp_err_t run_init_sequence(st7305_t *d)
{
    /* Panel tuning values - see the header comment for provenance. */
    static const uint8_t nvm_load[]   = { 0x17, 0x02 };
    static const uint8_t booster[]    = { 0x01 };
    static const uint8_t gate_volt[]  = { 0x11, 0x04 };
    static const uint8_t vshp[]       = { 0x69, 0x69, 0x69, 0x69 };
    static const uint8_t vslp[]       = { 0x19, 0x19, 0x19, 0x19 };
    static const uint8_t vshn[]       = { 0x4B, 0x4B, 0x4B, 0x4B };
    static const uint8_t vsln[]       = { 0x19, 0x19, 0x19, 0x19 };
    static const uint8_t osc[]        = { 0x80, 0xE9 };
    static const uint8_t frame_rate[] = { 0x02 };
    static const uint8_t gate_hpm[]   = { 0xE5, 0xF6, 0x05, 0x46, 0x77,
                                          0x77, 0x77, 0x77, 0x76, 0x45 };
    static const uint8_t gate_lpm[]   = { 0x05, 0x46, 0x77, 0x77,
                                          0x77, 0x77, 0x76, 0x45 };
    static const uint8_t gamma[]      = { 0x32, 0x03, 0x1F };
    static const uint8_t src_set[]    = { 0x13 };
    static const uint8_t duty[]       = { 0x64 };   /* multiplex ratio 100 */
    static const uint8_t clr_ram[]    = { 0x00 };
    static const uint8_t madctl[]     = { 0x48 };   /* portrait, data order */
    static const uint8_t colmod[]     = { 0x11 };   /* 1 bpp block format   */
    static const uint8_t src_volt[]   = { 0x20 };
    static const uint8_t panel_set[]  = { 0x29 };
    static const uint8_t teon[]       = { 0x00 };
    static const uint8_t autopwr[]    = { 0xFF };

    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_NVM_LOAD,   nvm_load,   2),  TAG, "nvm");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_BOOSTER,    booster,    1),  TAG, "boost");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_GATE_VOLT,  gate_volt,  2),  TAG, "gate");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_VSHP,       vshp,       4),  TAG, "vshp");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_VSLP,       vslp,       4),  TAG, "vslp");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_VSHN,       vshn,       4),  TAG, "vshn");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_VSLN,       vsln,       4),  TAG, "vsln");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_OSC,        osc,        2),  TAG, "osc");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_FRAME_RATE, frame_rate, 1),  TAG, "fr");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_GATE_HPM,   gate_hpm,   10), TAG, "ghpm");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_GATE_LPM,   gate_lpm,   8),  TAG, "glpm");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_GAMMA,      gamma,      3),  TAG, "gamma");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_SRC_SET,    src_set,    1),  TAG, "src");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_DUTY,       duty,       1),  TAG, "duty");

    ESP_RETURN_ON_ERROR(cmd(d, CMD_SLPOUT), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_CLR_RAM,   clr_ram,   1), TAG, "clrram");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_MADCTL,    madctl,    1), TAG, "madctl");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_COLMOD,    colmod,    1), TAG, "colmod");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_SRC_VOLT,  src_volt,  1), TAG, "srcvolt");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_PANEL_SET, panel_set, 1), TAG, "panel");
    ESP_RETURN_ON_ERROR(cmd(d, CMD_INVON), TAG, "invon");

    ESP_RETURN_ON_ERROR(set_window(d), TAG, "window");

    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_TEON,    teon,    1), TAG, "teon");
    ESP_RETURN_ON_ERROR(cmd_data(d, CMD_AUTOPWR, autopwr, 1), TAG, "autopwr");
    ESP_RETURN_ON_ERROR(cmd(d, CMD_HPM),    TAG, "hpm");
    ESP_RETURN_ON_ERROR(cmd(d, CMD_DISPON), TAG, "dispon");
    return ESP_OK;
}

esp_err_t st7305_new(int spi_host, int clk, int mosi, int cs, int dc, int rst,
                     int pclk_hz, st7305_t **out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    st7305_t *d = calloc(1, sizeof *d);
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no mem for device");

    /* The panel framebuffer is small enough for internal RAM, and keeping it
     * there makes the per-refresh DMA transfer straightforward. */
    d->fb = heap_caps_malloc(ST7305_FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (d->fb == NULL) {
        free(d);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "no mem for framebuffer");
    }
    memset(d->fb, 0xFF, ST7305_FB_BYTES);   /* all paper */
    d->rst_gpio = rst;
    d->spi_host = -1;   /* 0 is a real host, so "none" has to be explicit */

    /* Everything from here can fail, and every failure unwinds through `fail:`
     * so the init path stays retryable rather than leaking 15 kB a go. */
    esp_err_t ret;   /* ESP_GOTO_ON_ERROR writes to a local named `ret` */

    const spi_bus_config_t bus = {
        .sclk_io_num     = clk,
        .mosi_io_num     = mosi,
        .miso_io_num     = -1,          /* write-only panel */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ST7305_FB_BYTES + 64,
    };
    ret = spi_bus_initialize(spi_host, &bus, SPI_DMA_CH_AUTO);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "spi bus");
    d->spi_host = spi_host;

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = cs,
        .dc_gpio_num       = dc,
        .spi_mode          = 0,
        .pclk_hz           = pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(intptr_t)spi_host,
                                   &io_cfg, &d->io);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "panel io");

    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << rst,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rst_cfg);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "rst gpio");

    reset_panel(d);
    ret = run_init_sequence(d);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "init sequence");

    ESP_LOGI(TAG, "up: %dx%d, %d byte framebuffer, %d Hz",
             ST7305_WIDTH, ST7305_HEIGHT, ST7305_FB_BYTES, pclk_hz);
    *out = d;
    return ESP_OK;

fail:
    st7305_delete(d);
    return ret;
}

void st7305_delete(st7305_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->io != NULL) {
        esp_lcd_panel_io_del(d->io);
    }
    if (d->spi_host >= 0) {
        spi_bus_free(d->spi_host);
    }
    heap_caps_free(d->fb);
    free(d);
}

uint8_t *st7305_framebuffer(st7305_t *d) { return d->fb; }

void st7305_clear(st7305_t *d, bool white)
{
    memset(d->fb, white ? 0xFF : 0x00, ST7305_FB_BYTES);
}

esp_err_t st7305_refresh(st7305_t *d)
{
    ESP_RETURN_ON_ERROR(set_window(d), TAG, "window");
    /* tx_color drives DC low for the command then high for the payload. */
    return esp_lcd_panel_io_tx_color(d->io, CMD_RAMWR, d->fb, ST7305_FB_BYTES);
}

esp_err_t st7305_set_low_power(st7305_t *d, bool low_power)
{
    return cmd(d, low_power ? CMD_LPM : CMD_HPM);
}

esp_err_t st7305_display_on(st7305_t *d, bool on)
{
    return cmd(d, on ? CMD_DISPON : CMD_DISPOFF);
}

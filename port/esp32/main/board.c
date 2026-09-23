#include "board.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "st7305.h"
#include "ui.h"

static const char *TAG = "board";

#define KEY_LONG_PRESS_US   (800 * 1000)
#define KEY_DEBOUNCE_US     (25 * 1000)

/* LVGL renders RGB565: two bytes per pixel, full screen. */
#define LV_FB_BYTES (UI_HOR_RES * UI_VER_RES * 2)

static st7305_t *g_panel;
static uint8_t  *g_lv_fb;

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * Which way the landscape image is rotated onto the portrait glass.
 *
 * The panel scans natively as 300 x 400. The UI is 400 x 300, so every pixel is
 * turned a quarter turn on its way out. Which quarter turn depends on how the
 * board ends up mounted, and that cannot be known until there is a board:
 * flip this to 0 at bring-up if the picture comes out upside down.
 */
#define BOARD_ROTATE_CW 1

/*
 * Reduce LVGL's anti-aliased RGB565 output to the panel's one bit, rotate it,
 * and place each pixel into the ST7305 block layout.
 *
 * Three conversions, none of which can be a memcpy:
 *   - colour to ink, via ui_px_is_paper() - the same rule the host simulator
 *     applies, so the two render identically;
 *   - landscape to the panel's native portrait;
 *   - linear rows to the panel's 4-wide x 2-tall byte blocks.
 *
 * 120000 pixels of shifts and a compare, a few milliseconds against a panel we
 * refresh twice a second.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t   w   = area->x2 - area->x1 + 1;
    const uint16_t *src = (const uint16_t *)(const void *)px_map;
    uint8_t        *fb  = st7305_framebuffer(g_panel);

    for (int32_t ly = area->y1; ly <= area->y2 && ly < UI_VER_RES; ly++) {
        const int32_t row = ly - area->y1;
        for (int32_t lx = area->x1; lx <= area->x2 && lx < UI_HOR_RES; lx++) {
            const uint16_t px = src[row * w + (lx - area->x1)];

#if BOARD_ROTATE_CW
            const uint16_t panel_x = (uint16_t)(ST7305_WIDTH - 1 - ly);
            const uint16_t panel_y = (uint16_t)lx;
#else
            const uint16_t panel_x = (uint16_t)ly;
            const uint16_t panel_y = (uint16_t)(ST7305_HEIGHT - 1 - lx);
#endif
            st7305_set_pixel(fb, panel_x, panel_y, ui_px_is_paper(px));
        }
    }

    if (lv_display_flush_is_last(disp)) {
        st7305_refresh(g_panel);
    }
    lv_display_flush_ready(disp);
}

lv_display_t *board_display_init(void)
{
    /*
     * 240 kB will not fit in internal RAM alongside everything else. Claimed
     * before the panel so that failing here has nothing to unwind.
     */
    g_lv_fb = heap_caps_malloc(LV_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_lv_fb == NULL) {
        ESP_LOGE(TAG, "no memory for the LVGL framebuffer");
        return NULL;
    }

    esp_err_t err = st7305_new(BOARD_LCD_SPI_HOST, BOARD_LCD_CLK, BOARD_LCD_MOSI,
                               BOARD_LCD_CS, BOARD_LCD_DC, BOARD_LCD_RST,
                               BOARD_LCD_PCLK_HZ, &g_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        heap_caps_free(g_lv_fb);
        g_lv_fb = NULL;
        return NULL;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(UI_HOR_RES, UI_VER_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_lv_fb, NULL, LV_FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    ESP_LOGI(TAG, "LVGL up at %dx%d landscape -> %dx%d panel, 1 bpp",
             UI_HOR_RES, UI_VER_RES, ST7305_WIDTH, ST7305_HEIGHT);
    return disp;
}

/* ---- LVGL task ------------------------------------------------------------
 *
 * LVGL gets its own task so that a repaint - roughly 120000 pixel conversions
 * plus a 15 kB SPI transfer - cannot stall the CAN poll loop in app_main. A
 * stalled poll loop would show up as packs going NO DATA for no reason.
 *
 * Pinned to core 1, away from app_main and (later) the Wi-Fi stack, both of
 * which live on core 0. Priority is deliberately low: nothing about drawing is
 * urgent on a panel refreshed twice a second.
 */

#define LVGL_TASK_STACK       (8 * 1024)
#define LVGL_TASK_PRIO        2
#define LVGL_TASK_CORE        1
#define LVGL_TASK_MIN_DELAY_MS 5
#define LVGL_TASK_MAX_DELAY_MS 500

static SemaphoreHandle_t g_lvgl_mux;

bool board_lvgl_lock(int timeout_ms)
{
    if (g_lvgl_mux == NULL) {
        return false;   /* not started yet: the caller owns LVGL outright */
    }
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY
                                              : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(g_lvgl_mux, ticks) == pdTRUE;
}

void board_lvgl_unlock(void)
{
    if (g_lvgl_mux != NULL) {
        xSemaphoreGiveRecursive(g_lvgl_mux);
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task running on core %d", xPortGetCoreID());

    uint32_t delay_ms = LVGL_TASK_MIN_DELAY_MS;
    while (true) {
        if (board_lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            board_lvgl_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool board_lvgl_start(void)
{
    g_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    if (g_lvgl_mux == NULL) {
        ESP_LOGE(TAG, "could not create the LVGL mutex");
        return false;
    }
    if (xTaskCreatePinnedToCore(lvgl_task, "LVGL", LVGL_TASK_STACK, NULL,
                                LVGL_TASK_PRIO, NULL, LVGL_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "could not start the LVGL task");
        vSemaphoreDelete(g_lvgl_mux);
        g_lvgl_mux = NULL;
        return false;
    }
    return true;
}

void board_key_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_KEY_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool board_key_poll(bool *long_press)
{
    static bool     was_down;
    static int64_t  down_at;
    static int64_t  last_edge;

    const bool    down = gpio_get_level(BOARD_KEY_GPIO) == 0;  /* active low */
    const int64_t now  = esp_timer_get_time();

    if (down == was_down || now - last_edge < KEY_DEBOUNCE_US) {
        return false;
    }
    last_edge = now;
    was_down  = down;

    if (down) {
        down_at = now;
        return false;             /* report on release, so we can time the hold */
    }
    *long_press = (now - down_at) >= KEY_LONG_PRESS_US;
    return true;
}

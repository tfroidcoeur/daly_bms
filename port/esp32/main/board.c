#include "board.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "driver/rtc_io.h"
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
/*
 * How long the LVGL task may sleep when it has nothing to do. This is the
 * dominant term in how quickly a press appears on screen: the press is latched
 * within 2 ms and the widgets are dirtied immediately, but nothing is drawn
 * until this task next wakes. At 500 ms that felt broken.
 *
 * 30 ms costs about 33 wakeups a second of a task that immediately finds
 * nothing to do - microseconds each - and bounds the input-to-ink delay at
 * roughly this plus one repaint.
 */
#define LVGL_TASK_MAX_DELAY_MS 30

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

/* ---- input: an external button, plus the onboard KEY --------------------- */

/* One mechanical button: debounce and the press timer need memory between
 * polls. */
typedef struct {
    gpio_num_t gpio;
    bool       was_down;
    bool       swallow;    /* the press that woke us: its release is no input */
    int64_t    down_at;
    int64_t    last_edge;
} btn_t;

static btn_t g_btn = { .gpio = BOARD_BTN_GPIO };

#if !(defined(CONFIG_BMS_CAN_RS_GPIO) && CONFIG_BMS_CAN_RS_GPIO == BOARD_KEY_GPIO)
/* GPIO18 is the KEY only while it is not the transceiver's Rs line. */
#define HAVE_KEY 1
static btn_t g_key = { .gpio = BOARD_KEY_GPIO };
#endif

static void btn_init(btn_t *b)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << b->gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /*
     * Already down at boot: this is the press that woke us. The wake alone
     * brings the screen up, so its release must not also turn a page - the
     * same rule as the first press on a sleeping screen.
     */
    if (gpio_get_level(b->gpio) == 0) {
        b->was_down  = true;
        b->swallow   = true;
        b->last_edge = esp_timer_get_time();
    }
}

void board_key_init(void)
{
    /* Deep sleep handed the button pin to the RTC mux for the wake-up; take
     * it back so the ordinary GPIO input works. */
    rtc_gpio_deinit(BOARD_BTN_GPIO);
    btn_init(&g_btn);
#ifdef HAVE_KEY
    btn_init(&g_key);
#endif
}

/* Debounced, reported on release so the hold can be timed. */
static bool btn_poll(btn_t *b, int64_t now, bool *long_press)
{
    const bool down = gpio_get_level(b->gpio) == 0;  /* active low */

    if (down == b->was_down || now - b->last_edge < KEY_DEBOUNCE_US) {
        return false;
    }
    b->last_edge = now;
    b->was_down  = down;

    if (down) {
        b->down_at = now;
        return false;             /* report on release, so we can time the hold */
    }
    if (b->swallow) {
        b->swallow = false;
        return false;
    }
    *long_press = (now - b->down_at) >= KEY_LONG_PRESS_US;
    return true;
}

bool board_key_poll(bool *long_press)
{
    const int64_t now = esp_timer_get_time();

    if (btn_poll(&g_btn, now, long_press)) {
        return true;
    }
#ifdef HAVE_KEY
    return btn_poll(&g_key, now, long_press);
#else
    return false;
#endif
}

/* ---- remote reporting: sleep and wake ---------------------------------- */

#if defined(CONFIG_BMS_CAN_RS_GPIO) && CONFIG_BMS_CAN_RS_GPIO >= 0
#define RS_GPIO ((gpio_num_t)CONFIG_BMS_CAN_RS_GPIO)
#endif

/* The panel's control pins: held through deep sleep so the controller sees
 * no reset and no stray clock, and keeps what it shows. */
static const gpio_num_t k_panel_pins[] = {
    BOARD_LCD_CLK, BOARD_LCD_MOSI, BOARD_LCD_CS, BOARD_LCD_DC, BOARD_LCD_RST,
};

remote_wake_t board_wake_cause(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return REMOTE_WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_EXT1:
        /* The button and CAN RX share ext1; the status says which fired. */
        return (esp_sleep_get_ext1_wakeup_status() & (1ULL << BOARD_BTN_GPIO))
                   ? REMOTE_WAKE_TOUCH : REMOTE_WAKE_CAN;
    default:                     return REMOTE_WAKE_BOOT;
    }
}

void board_release_holds(void)
{
    for (size_t i = 0; i < sizeof k_panel_pins / sizeof k_panel_pins[0]; i++) {
        gpio_hold_dis(k_panel_pins[i]);
    }
    gpio_deep_sleep_hold_dis();
}

void board_can_standby(bool standby)
{
#ifdef RS_GPIO
    gpio_hold_dis(RS_GPIO);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RS_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(RS_GPIO, standby ? 1 : 0);
#else
    (void)standby;
#endif
}

void board_deep_sleep(uint32_t seconds)
{
    /* Transceiver to standby: its receiver still follows the bus, so any
     * frame on it pulls RX low and wakes us. */
    board_can_standby(true);
#ifdef RS_GPIO
    gpio_hold_en(RS_GPIO);
#endif

    for (size_t i = 0; i < sizeof k_panel_pins / sizeof k_panel_pins[0]; i++) {
        gpio_hold_en(k_panel_pins[i]);
    }
    gpio_deep_sleep_hold_en();

    /*
     * RX idles high (recessive); any frame on the bus drives it low. The
     * button pulls its pin low too. The transceiver drives RX, but the button
     * pin floats without its pull-up, which in deep sleep is the RTC one - and
     * that needs the RTC peripherals kept powered.
     */
    rtc_gpio_pullup_en(BOARD_BTN_GPIO);
    rtc_gpio_pulldown_dis(BOARD_BTN_GPIO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io((1ULL << BOARD_CAN_RX) | (1ULL << BOARD_BTN_GPIO),
                                    ESP_EXT1_WAKEUP_ANY_LOW);

    if (seconds != UINT32_MAX) {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }
    ESP_LOGI(TAG, "deep sleep %s", seconds == UINT32_MAX ? "until button or bus" : "");
    esp_deep_sleep_start();
}

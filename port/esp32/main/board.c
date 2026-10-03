#include "board.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "driver/touch_sens.h"
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

/* ---- input: the onboard KEY, plus two capacitive pads -------------------- */

static touch_sensor_handle_t g_touch;

/* One pad: its channel, how hard it must be pressed, and where it is in the
 * press - debounce and hysteresis both need memory between polls. */
typedef struct {
    const char            *name;
    touch_channel_handle_t chan;
    uint32_t               press_pm;    /* BOARD_TOUCH_*_PERMILLE */
    bool                   active;      /* latched until the release edge */
    bool                   pending;     /* above threshold, not yet for long enough */
    int64_t                above_since; /* us, valid while pending */
    int64_t                tracked_at;  /* us, last poll the benchmark was tracking */
} pad_t;

static pad_t g_pad_next  = { .name = "next ", .press_pm = BOARD_TOUCH_NEXT_PERMILLE  };
static pad_t g_pad_drill = { .name = "drill", .press_pm = BOARD_TOUCH_DRILL_PERMILLE };

/* Above this delta the hardware stops tracking the benchmark. */
static uint32_t freeze_edge(uint32_t press)
{
    return press * BOARD_TOUCH_FREEZE_PCT / 100;
}

/* The two edges a pad's delta is judged against, for a given benchmark. */
static void pad_edges(const pad_t *p, uint32_t benchmark, uint32_t *press,
                      uint32_t *release)
{
    *press   = benchmark * p->press_pm / 1000;
    *release = *press * BOARD_TOUCH_RELEASE_PCT / 100;
}

/*
 * Hand the hardware a real active threshold: the pad's freeze edge, from the
 * benchmark just measured.
 *
 * The benchmark filter stops tracking while the hardware considers a channel
 * active, so a held finger is not calibrated away. With a threshold of 0 every
 * reading above the benchmark counts as active - which is every other reading -
 * and the benchmark froze at its first value and never moved again: `drill`
 * crept 360 counts above it in minutes, over a third of the way to a press.
 * At BOARD_TOUCH_FREEZE_PCT it tracks drift whenever nobody is near, and holds
 * still from the moment a finger approaches.
 *
 * Reconfiguring needs the controller disabled, and resets the benchmark; the
 * caller rescans afterwards.
 */
static void pad_set_hw_thresh(pad_t *p, const touch_channel_config_t *base)
{
    uint32_t benchmark = 0, press, release;

    touch_channel_read_data(p->chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark);
    pad_edges(p, benchmark, &press, &release);

    touch_channel_config_t cfg = *base;
    cfg.active_thresh[0] = freeze_edge(press);
    ESP_ERROR_CHECK(touch_sensor_reconfig_channel(p->chan, &cfg));
}

/*
 * Bring up the touch controller for the two pads.
 *
 * Failure here is not fatal: the pads are copper tape behind the front panel
 * and may simply not be fitted. The KEY button drives the same UI either way,
 * so a board without pads is a board with one button rather than a broken one.
 */
static void touch_init(void)
{
    touch_sensor_sample_config_t sample[1] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5,
                                              TOUCH_VOLT_LIM_H_2V2),
    };
    const touch_sensor_config_t sens = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample);

    if (touch_sensor_new_controller(&sens, &g_touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch controller; KEY only");
        g_touch = NULL;
        return;
    }

    /*
     * The filter is what makes a bare pad usable at all. It tracks a slowly
     * moving benchmark per channel - the reading with nobody near it - so the
     * drift from temperature, humidity and a settling enclosure is absorbed
     * rather than mistaken for a finger, which arrives far faster than any of
     * those. The benchmark freezes while a channel is active, or a held touch
     * would be calibrated away under your fingertip.
     */
    const touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(g_touch, &filter));

    const touch_channel_config_t chan = {
        /* A placeholder until there is a benchmark to set it from - see
         * pad_set_hw_thresh(). Presses are still judged in board_key_poll(). */
        .active_thresh = { 0 },
        /*
         * These two must be set explicitly. Zero is a legal value for both and
         * means something quite different from "default": TOUCH_CHARGE_SPEED_0
         * is "no charge, always zero", which leaves the pad never charged, the
         * measurement never finishing, and the counter pinned at its maximum
         * (0x3FFFFF) on every read.
         */
        .charge_speed     = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    if (touch_sensor_new_channel(g_touch, BOARD_TOUCH_NEXT_CHAN, &chan,
                                 &g_pad_next.chan) != ESP_OK ||
        touch_sensor_new_channel(g_touch, BOARD_TOUCH_DRILL_CHAN, &chan,
                                 &g_pad_drill.chan) != ESP_OK) {
        ESP_LOGW(TAG, "touch channels unavailable; KEY only");
        g_touch = NULL;
        return;
    }

    /*
     * Scan a few times before trusting anything: the benchmark starts from
     * whatever the first measurement happens to be, and one sample of that is
     * not a baseline.
     */
    ESP_ERROR_CHECK(touch_sensor_enable(g_touch));
    for (int i = 0; i < 8; i++) {
        touch_sensor_trigger_oneshot_scanning(g_touch, 2000);
    }

    ESP_ERROR_CHECK(touch_sensor_disable(g_touch));
    pad_set_hw_thresh(&g_pad_next, &chan);
    pad_set_hw_thresh(&g_pad_drill, &chan);
    ESP_ERROR_CHECK(touch_sensor_enable(g_touch));
    for (int i = 0; i < 8; i++) {
        touch_sensor_trigger_oneshot_scanning(g_touch, 2000);
    }
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(g_touch));

    /* These two numbers are what the BOARD_TOUCH_*_PERMILLE thresholds have to be
     * chosen against for real pads. */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t bm_next = 0, bm_drill = 0;
    touch_channel_read_data(g_pad_next.chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                            &bm_next);
    touch_channel_read_data(g_pad_drill.chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                            &bm_drill);
    ESP_LOGI(TAG, "touch up: pad benchmarks next=%" PRIu32 " (%d permille) drill=%"
                  PRIu32 " (%d permille)",
             bm_next, BOARD_TOUCH_NEXT_PERMILLE, bm_drill,
             BOARD_TOUCH_DRILL_PERMILLE);
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

    touch_init();
}

/*
 * Read one pad: how far its lightly-filtered reading sits above its own slowly
 * tracked benchmark, and the two thresholds that delta is judged against.
 * Relative, not absolute: the benchmark depends on pad size and overlay
 * thickness and moves with the weather, so an absolute figure would need
 * retuning for every build and every warm afternoon.
 */
static bool pad_read(const pad_t *p, uint32_t *delta, uint32_t *press,
                     uint32_t *release, uint32_t *benchmark)
{
    uint32_t smooth = 0;

    if (p->chan == NULL ||
        touch_channel_read_data(p->chan, TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                &smooth) != ESP_OK ||
        touch_channel_read_data(p->chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                benchmark) != ESP_OK) {
        return false;
    }
    *delta = smooth > *benchmark ? smooth - *benchmark : 0;
    pad_edges(p, *benchmark, press, release);
    return true;
}

/*
 * True once per touch, on the edge where it is accepted.
 *
 * Four rules, each for a different way a pad lies:
 *   - its own threshold, because two pads need not land a touch equally hard;
 *   - debounce: above the threshold for BOARD_TOUCH_DEBOUNCE_MS unbroken before
 *     it counts, so a brush or a spike on the lead is not a press;
 *   - hysteresis: once active, it stays active until the delta falls to
 *     BOARD_TOUCH_RELEASE_PCT of the threshold, so a finger hovering at the
 *     edge is one press rather than a burst;
 *   - a stuck guard: a benchmark held frozen for BOARD_TOUCH_STUCK_MS is reset,
 *     so something resting on the pad cannot leave it dead or half-blind.
 */
static bool pad_pressed(pad_t *p, int64_t now)
{
    uint32_t delta, press, release, benchmark;

    if (!pad_read(p, &delta, &press, &release, &benchmark)) {
        return false;
    }

    if (delta <= freeze_edge(press)) {
        p->tracked_at = now;
    } else if (now - p->tracked_at > (int64_t)BOARD_TOUCH_STUCK_MS * 1000) {
        /* The new benchmark is the reading as it stands, so the delta drops to
         * zero and an active pad releases on the next poll. If a finger really
         * was still there, lifting it reads below the benchmark, which the
         * filter follows at once. */
        const touch_chan_benchmark_config_t reset = { .do_reset = true };
        touch_channel_config_benchmark(p->chan, &reset);
        ESP_LOGW(TAG, "touch: %s benchmark frozen for %d s, reset", p->name,
                 BOARD_TOUCH_STUCK_MS / 1000);
        p->tracked_at = now;
        p->pending    = false;
        return false;
    }

    if (p->active) {
        if (delta < release) {
            p->active = false;
        }
        return false;
    }

    if (delta <= press) {
        p->pending = false;                  /* dipped: start the clock again */
        return false;
    }
    if (!p->pending) {
        p->pending     = true;
        p->above_since = now;
        return false;
    }
    if (now - p->above_since < (int64_t)BOARD_TOUCH_DEBOUNCE_MS * 1000) {
        return false;
    }
    p->pending = false;
    p->active  = true;
    return true;
}

void board_touch_report(void)
{
    if (g_touch == NULL) {
        ESP_LOGE(TAG, "touch controller did not start - KEY only");
        return;
    }

    const pad_t *pads[] = { &g_pad_next, &g_pad_drill };

    for (unsigned i = 0; i < sizeof pads / sizeof pads[0]; i++) {
        uint32_t delta, press, release, benchmark;
        if (!pad_read(pads[i], &delta, &press, &release, &benchmark)) {
            ESP_LOGE(TAG, "  %s: channel unavailable", pads[i]->name);
            continue;
        }
        /* "press" is the edge a touch must clear, "release" the one it must
         * fall back under - so a hover shows as a delta between the two. */
        ESP_LOGI(TAG, "  %s bench %7" PRIu32 "  delta %7" PRIu32
                      "  press %6" PRIu32 "  release %6" PRIu32 "  %s",
                 pads[i]->name, benchmark, delta, press, release,
                 delta > press   ? "** TOUCHED **"
                 : delta > release ? "(between edges)" : "");
    }
}

bool board_key_poll(bool *long_press)
{
    static bool     was_down;
    static int64_t  down_at;
    static int64_t  last_edge;

    const int64_t now = esp_timer_get_time();

    /*
     * Pads first, and they report on touch rather than release: there is no
     * hold to time. Each pad maps onto one of the two actions the UI already
     * has, so a second pad replaces the 800 ms hold rather than adding
     * anything for ui.c to learn.
     */
    if (pad_pressed(&g_pad_next, now)) {
        *long_press = false;
        return true;
    }
    if (pad_pressed(&g_pad_drill, now)) {
        *long_press = true;
        return true;
    }

    const bool down = gpio_get_level(BOARD_KEY_GPIO) == 0;  /* active low */

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

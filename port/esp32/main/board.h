/*
 * ESP32-S3-RLCD-4.2 board definition.
 *
 * Pin numbers come from the Waveshare schematic and the Zephyr board port,
 * which agree. See docs/hardware/esp32-s3-rlcd-4.2.md.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "lvgl.h"

/* --- reflective LCD, ST7305 over SPI2 ------------------------------------- */
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_CLK        GPIO_NUM_11
#define BOARD_LCD_MOSI       GPIO_NUM_12
#define BOARD_LCD_CS         GPIO_NUM_40
#define BOARD_LCD_DC         GPIO_NUM_5
#define BOARD_LCD_RST        GPIO_NUM_41
#define BOARD_LCD_PCLK_HZ    (10 * 1000 * 1000)   /* Zephyr uses 10 MHz */

/* --- buttons -------------------------------------------------------------- */
#define BOARD_KEY_GPIO       GPIO_NUM_18          /* active low, pull-up */
#define BOARD_BOOT_GPIO      GPIO_NUM_0

/* --- I2C: PCF85063 RTC (0x51) and SHTC3 (0x70) ---------------------------- */
#define BOARD_I2C_SDA        GPIO_NUM_13
#define BOARD_I2C_SCL        GPIO_NUM_14

/* --- battery sense: 100k/200k divider on ADC1 channel 3 ------------------- */
#define BOARD_VBAT_GPIO      GPIO_NUM_4

/*
 * --- CAN and touch ----------------------------------------------------------
 *
 * The 2 x 8 expansion header (P1) brings out GPIO0, 1, 2, 3, 17 and 18. GPIO0
 * is the BOOT strap and GPIO18 drives the KEY button, leaving 1, 2, 3 and 17.
 *
 * Touch gets first pick and CAN takes what is left, because the constraint runs
 * one way: the touch channels are wired to GPIO1..GPIO14 in silicon, while TWAI
 * routes through the GPIO matrix and will work on any pin.
 *
 * GPIO3 is nominally the JTAG-select strap, but that function only exists once
 * the JTAG_SEL_ENABLE eFuse is burned, which it is not from the factory. It is
 * used for CAN RX rather than TX so the pin is held at a defined level at boot:
 * the transceiver's R output idles high (recessive).
 */
#define BOARD_CAN_TX         GPIO_NUM_17          /* P1 pin 13 -> transceiver D */
#define BOARD_CAN_RX         GPIO_NUM_3           /* P1 pin 11 <- transceiver R */

/*
 * Two capacitive pads, read by the SoC's own touch peripheral - a copper area
 * behind the front panel, no controller IC. Pad numbers match the GPIO on this
 * part (TOUCH_PAD_NUMn is GPIOn for n = 1..14).
 *
 * Two pads rather than one means the cell page gets its own control instead of
 * an 800 ms hold on a single button: a hold gives no feedback while you wait,
 * and this panel redraws too slowly to show any.
 */
#define BOARD_TOUCH_NEXT_CHAN   1                 /* P1 pin 7  - next page   */
#define BOARD_TOUCH_DRILL_CHAN  2                 /* P1 pin 9  - cell detail */

/*
 * Activation thresholds, one per pad, as a percentage of that pad's own
 * benchmark: a pad goes active when (smooth - benchmark) exceeds benchmark *
 * PCT / 100. Relative rather than absolute because the benchmark depends on pad
 * size and overlay thickness, and drifts with temperature and humidity - the
 * driver tracks that drift for us.
 *
 * Higher = less sensitive. board_key_init() logs each pad's benchmark at boot,
 * and the touch-monitor build mode prints the live delta against both edges, so
 * these are set from measurement rather than taste.
 *
 * Measured on bare copper tape, no overlay, 2026-09-26:
 *
 *            benchmark   touched    crosstalk from the other pad
 *   next        30200    +190000    -
 *   drill       28300    +5200      +242
 *
 * Per pad, because the pads differ by a factor of 36 in how hard a touch lands.
 * A single shared 2 % left `next` firing about 300x over on a real touch, which
 * is another way of saying it fired on a hand passing nearby. At 20 % it still
 * clears a touch 30x over. `drill` is the weak pad: at 5 % it needs 1415, which
 * a touch clears 3.7x over and the +242 of crosstalk from `next` stays 5.8x
 * below.
 *
 * These are for bare copper. An overlay - tape, the enclosure wall - shrinks
 * every number in the table, touched deltas most of all, so re-measure with the
 * touch monitor once it is fitted: expect both percentages to come down.
 */
#define BOARD_TOUCH_NEXT_PCT    20
#define BOARD_TOUCH_DRILL_PCT    5

/*
 * Hysteresis: once active, a pad stays active until its delta falls below this
 * percentage of its activation threshold. Without it a finger that settles near
 * the threshold - arriving, leaving, or resting lightly - crosses it several
 * times and reads as several presses.
 */
#define BOARD_TOUCH_RELEASE_PCT 50

/*
 * Debounce: a pad must read above its threshold continuously for this long
 * before it counts. Rejects a brush, a sleeve, a spike on the lead wire. Well
 * under what a deliberate press takes, and the panel redraws far slower than
 * this anyway, so it cannot be felt.
 */
#define BOARD_TOUCH_DEBOUNCE_MS 60

/* Initialise SPI, the ST7305 panel, and LVGL. Returns the LVGL display. */
lv_display_t *board_display_init(void);

/*
 * Start the LVGL task.
 *
 * LVGL is not thread-safe, so after this call every lv_*() and ui_*() call -
 * from any task, including this one - must be wrapped in board_lvgl_lock() /
 * board_lvgl_unlock(). Call it once, after board_display_init().
 */
bool board_lvgl_start(void);

/*
 * Take the LVGL lock. `timeout_ms` < 0 waits indefinitely. Returns false if the
 * lock could not be taken, in which case you must NOT touch LVGL.
 *
 * The mutex is recursive, so nesting locks in a call chain is safe.
 */
bool board_lvgl_lock(int timeout_ms);
void board_lvgl_unlock(void);

/* Configure the KEY button. Poll it with board_key_poll(). */
void board_key_init(void);

/*
 * Log both touch pads' benchmark, smoothed reading and the margin to the
 * threshold. For the touch-monitor build mode: touch a pad and watch whether
 * the numbers move at all, which separates a dead pad from a wrong threshold.
 */
void board_touch_report(void);

/*
 * Debounced button poll with press-length classification.
 * Returns true once per release, with `long_press` set for holds over 800 ms.
 */
bool board_key_poll(bool *long_press);

#endif /* BOARD_H */

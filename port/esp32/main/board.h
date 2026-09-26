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
 * Activation threshold, as a fraction of each pad's own benchmark: active when
 * (smooth - benchmark) > benchmark / N. Relative rather than absolute because
 * the benchmark depends on pad size and overlay thickness, and drifts with
 * temperature and humidity - the driver tracks that drift for us.
 *
 * Lower N = more sensitive. board_key_init() logs each pad's benchmark at boot,
 * and the touch-monitor build mode prints the live margin, so this is set from
 * measurement rather than taste.
 *
 * Measured on copper tape, 2026-09-26:
 *
 *            benchmark   touched    crosstalk from the other pad
 *   next        30200    +190000    -
 *   drill       28300    +5200      +242
 *
 * Crosstalk sets the floor, not noise. Touching `next` puts +242 on `drill`,
 * so drill's threshold has to stay well clear of that - which at benchmark
 * 28300 means roughly 500 or more. At 2 % it needs 566: a light touch still
 * clears it about 9x over, and the crosstalk stays 2.3x below it.
 *
 * Going further is a hardware job rather than a tuning one. 1 % would need 283
 * against 242 of interference, which is not margin. To get more sensitivity
 * than this, move the pads further apart or enlarge the weaker one - drill
 * reads +5200 on a touch where next reads +190000, so it is the pad limiting
 * this, not the number below.
 */
#define BOARD_TOUCH_THRESH_DIV  50                /* 2 % of benchmark */

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

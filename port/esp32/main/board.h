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
#define BOARD_LCD_SPI_HOST SPI2_HOST
#define BOARD_LCD_CLK GPIO_NUM_11
#define BOARD_LCD_MOSI GPIO_NUM_12
#define BOARD_LCD_CS GPIO_NUM_40
#define BOARD_LCD_DC GPIO_NUM_5
#define BOARD_LCD_RST GPIO_NUM_41
#define BOARD_LCD_PCLK_HZ (10 * 1000 * 1000) /* Zephyr uses 10 MHz */

/* --- buttons -------------------------------------------------------------- */
#define BOARD_KEY_GPIO GPIO_NUM_18 /* active low, pull-up */
#define BOARD_BOOT_GPIO GPIO_NUM_0

/* --- I2C: PCF85063 RTC (0x51) and SHTC3 (0x70) ---------------------------- */
#define BOARD_I2C_SDA GPIO_NUM_13
#define BOARD_I2C_SCL GPIO_NUM_14

/* --- battery sense: 100k/200k divider on ADC1 channel 3 ------------------- */
#define BOARD_VBAT_GPIO GPIO_NUM_4

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
#define BOARD_CAN_TX GPIO_NUM_17 /* P1 pin 13 -> transceiver D */
#define BOARD_CAN_RX GPIO_NUM_3  /* P1 pin 11 <- transceiver R */

/*
 * Two capacitive pads, read by the SoC's own touch peripheral - a copper area
 * behind the front panel, no controller IC. Pad numbers match the GPIO on this
 * part (TOUCH_PAD_NUMn is GPIOn for n = 1..14).
 *
 * Two pads rather than one means the cell page gets its own control instead of
 * an 800 ms hold on a single button: a hold gives no feedback while you wait,
 * and this panel redraws too slowly to show any.
 */
#define BOARD_TOUCH_NEXT_CHAN 1  /* P1 pin 7  - next page   */
#define BOARD_TOUCH_DRILL_CHAN 2 /* P1 pin 9  - cell detail */

/*
 * Activation thresholds, one per pad, in per mille of that pad's own benchmark:
 * a pad goes active when (smooth - benchmark) exceeds benchmark * PERMILLE /
 * 1000. Relative rather than absolute because the benchmark depends on pad size
 * and overlay thickness, and drifts with temperature and humidity - the driver
 * tracks that drift for us. Per mille, not percent: behind a wall a press is
 * only a few percent, and 1 % steps are too coarse to place the edge.
 *
 * Higher = less sensitive. board_key_init() logs each pad's benchmark at boot,
 * and the touch-monitor build mode prints the live delta against both edges, so
 * these are set from measurement rather than taste.
 *
 * Measured through the PLA enclosure wall, 2026-10-02, benchmark ~31900 on both,
 * benchmark tracking as set up in touch_init():
 *
 *            idle   hover ~1 cm   slow approach   press / 8 s hold   other pad
 *   next     0-5      230-475         1132           1250-1335          <=5
 *   drill    0-5      350-540         1290           1340-1590          <=11
 *
 * The wall flattens the two pads to nearly the same signal - on bare copper they
 * differed 36-fold. 30 per mille puts `next` at ~955: a press clears it
 * 1.3-1.4x, a hover sits at half. `drill` at 25 per mille (~795) has a press
 * 1.7-2x over and a hover 0.7x under. A held press does not decay. Wider margins
 * are a mechanical job - a thinner wall over the pad, the copper flat against
 * it - not a firmware one.
 */
#define BOARD_TOUCH_NEXT_PERMILLE 30
#define BOARD_TOUCH_DRILL_PERMILLE 25

/*
 * Hysteresis: once active, a pad stays active until its delta falls below this
 * percentage of its activation threshold. Without it a finger that settles near
 * the threshold - arriving, leaving, or resting lightly - crosses it several
 * times and reads as several presses.
 */
#define BOARD_TOUCH_RELEASE_PCT 40

/*
 * Freeze edge, as a percentage of the activation threshold: the delta above
 * which the hardware stops tracking a pad's benchmark. It has to sit near the
 * noise, not near a press. The controller scans continuously and the benchmark
 * IIR follows within a few scans, so anything below this edge is calibrated
 * away almost as it happens - set at the release edge, it swallowed ~500 of a
 * ~1300 press and the whole of a slow approach. Idle noise is under 10, so 5 %
 * (~45) freezes the benchmark once a finger is a few cm off while still letting
 * it follow temperature drift, which is far slower.
 */
#define BOARD_TOUCH_FREEZE_PCT 5

/*
 * Debounce: a pad must read above its threshold continuously for this long
 * before it counts. Rejects a brush, a sleeve, a spike on the lead wire. Well
 * under what a deliberate press takes, and the panel redraws far slower than
 * this anyway, so it cannot be felt.
 */
#define BOARD_TOUCH_DEBOUNCE_MS 60

/*
 * A pad whose delta stays above the freeze edge for longer than this is not
 * being pressed - nobody holds a button for 15 s - but has something resting on
 * it: water on the wall, a cable against the pad, a reading still settling after
 * boot. The hardware does not track the benchmark while the delta is above the
 * freeze edge, so left alone it never recovers: above the press edge the pad is
 * dead after one phantom press, and below it the pad sits that much less
 * sensitive. Past this, the benchmark is reset to the present reading.
 */
#define BOARD_TOUCH_STUCK_MS 15000

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

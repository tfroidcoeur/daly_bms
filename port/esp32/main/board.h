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
 * --- CAN --------------------------------------------------------------------
 *
 * The 2 x 8 expansion header (P1) brings out only GPIO0, 1, 2, 3, 17 and 18.
 * GPIO0 and 3 are strapping pins and GPIO18 is the KEY button, which leaves
 * GPIO1, GPIO2 and GPIO17. CAN takes the first two - P1 pins 7 and 9, in the
 * same column as 3V3 (pin 1) and GND (pin 3).
 */
#define BOARD_CAN_TX         GPIO_NUM_1           /* P1 pin 7  -> transceiver D */
#define BOARD_CAN_RX         GPIO_NUM_2           /* P1 pin 9  <- transceiver R */

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
 * Debounced button poll with press-length classification.
 * Returns true once per release, with `long_press` set for holds over 800 ms.
 */
bool board_key_poll(bool *long_press);

#endif /* BOARD_H */

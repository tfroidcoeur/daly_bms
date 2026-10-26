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
 * --- CAN and the external button --------------------------------------------
 *
 * The 2 x 8 expansion header (P1) brings out GPIO0, 1, 2, 3, 17 and 18. GPIO0
 * is the BOOT strap and GPIO18 drives the KEY button, leaving 1, 2, 3 and 17.
 *
 * GPIO3 is nominally the JTAG-select strap, but that function only exists once
 * the JTAG_SEL_ENABLE eFuse is burned, which it is not from the factory. It is
 * used for CAN RX rather than TX so the pin is held at a defined level at boot:
 * the transceiver's R output idles high (recessive).
 */
#define BOARD_CAN_TX GPIO_NUM_17 /* P1 pin 13 -> transceiver D */
#define BOARD_CAN_RX GPIO_NUM_3  /* P1 pin 11 <- transceiver R */

/*
 * One mechanical push button on the front, between this pin and GND. It works
 * like the KEY: a short press turns the page, a hold opens cell detail, and a
 * press wakes the board from deep sleep. GPIO1 because it is on the header and
 * an RTC GPIO (ext1 wake-up), and is not a strap pin - a button held or wet at
 * power-up cannot stop the board from booting, as it could on GPIO0. GPIO2
 * (P1 pin 9) is free.
 */
#define BOARD_BTN_GPIO GPIO_NUM_1 /* P1 pin 7, active low, internal pull-up */

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

/* Configure the external button and, when it is not the Rs line, the KEY.
 * Poll them with board_key_poll(). */
void board_key_init(void);

/*
 * Debounced button poll with press-length classification, for both buttons.
 * Returns true once per release, with `long_press` set for holds over 800 ms.
 */
bool board_key_poll(bool *long_press);

/* ---- remote reporting: sleep and wake ---------------------------------- */

#include "remote.h"

/* Why this boot happened, in core/remote.h's terms. */
remote_wake_t board_wake_cause(void);

/*
 * The transceiver's Rs pin (CONFIG_BMS_CAN_RS_GPIO): low = normal, high =
 * standby. Releases any hold left from deep sleep. No-op when not wired.
 */
void board_can_standby(bool standby);

/*
 * Deep sleep until `seconds` pass (UINT32_MAX: no timer), the button is pressed,
 * or the CAN bus goes dominant. Puts the transceiver in standby and holds the
 * panel's pins so the "Zzz" on the glass survives. Does not return.
 */
void board_deep_sleep(uint32_t seconds);

/* Release the pin holds a deep sleep left; call before touching the panel. */
void board_release_holds(void);

#endif /* BOARD_H */

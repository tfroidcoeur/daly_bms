/*
 * ST7305 reflective LCD driver for the Waveshare ESP32-S3-RLCD-4.2 (300 x 400).
 *
 * Written for this board specifically. The register sequence is Waveshare's,
 * verified byte-for-byte - every command, parameter and ordering - against
 * their own ESP-IDF driver:
 *
 *   waveshareteam/ESP32-S3-RLCD-4.2 (Apache 2.0), file
 *   02_Example/ESP-IDF/09_LVGL_V9_Test/components/port_bsp/display_bsp.cpp
 *
 * It also agrees with the Zephyr board devicetree on every value it carries
 * (NVM load 17 02, gate voltages 11 04, VSHP 69, VSLP 19, VSHN 4B, VSLN 19,
 * oscillator 0x80, frame rate 0x02, duty 0x64, both gate waveforms), and with
 * their 10 MHz pixel clock and their pixel packing.
 *
 * Two deliberate departures from their driver: the panel framebuffer lives in
 * internal DMA-capable RAM rather than PSRAM (it is 15 kB, and that keeps the
 * per-refresh transfer a plain DMA write), and LVGL is single-buffered rather
 * than given their two full-screen PSRAM buffers - double buffering buys
 * nothing on a panel refreshed twice a second and costs 240 kB.
 *
 * Why not a stock component: the ST7305 packs a 4-wide x 2-tall block of pixels
 * into every byte, so the framebuffer is not a linear bitmap and no generic
 * esp_lcd panel driver can drive it. (The `leazer/esp_lcd_st7305` component on
 * the registry is a standalone driver hardcoded for a 2.9" 168x384 panel; it is
 * not an esp_lcd panel driver at all, despite the name.)
 */
#ifndef ST7305_H
#define ST7305_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ui.h"

#define ST7305_WIDTH   300
#define ST7305_HEIGHT  400

/*
 * One byte holds 4 columns x 2 rows. 300/4 = 75 bytes per row-pair,
 * 400/2 = 200 row-pairs.
 */
#define ST7305_STRIDE      (ST7305_WIDTH / 4)                    /* 75    */
#define ST7305_ROW_PAIRS   (ST7305_HEIGHT / 2)                   /* 200   */
#define ST7305_FB_BYTES    (ST7305_STRIDE * ST7305_ROW_PAIRS)    /* 15000 */

/* The UI is landscape, the glass is portrait: the two must be a quarter turn
 * apart or flush_cb would write outside the framebuffer. */
_Static_assert(ST7305_WIDTH == UI_VER_RES && ST7305_HEIGHT == UI_HOR_RES,
               "panel and UI geometry must differ by a 90 degree rotation");

typedef struct st7305_t st7305_t;

/* Bring up SPI, reset the panel and run the init sequence. */
esp_err_t st7305_new(int spi_host, int clk, int mosi, int cs, int dc, int rst,
                     int pclk_hz, st7305_t **out);

/* Release the panel, its SPI bus and its framebuffer. NULL is a no-op. */
void st7305_delete(st7305_t *dev);

/* The 15000-byte panel-format framebuffer. Write it with st7305_set_pixel(). */
uint8_t *st7305_framebuffer(st7305_t *dev);

/* Fill the framebuffer. `white` true = paper, false = ink. */
void st7305_clear(st7305_t *dev, bool white);

/*
 * Byte index and bit mask for a pixel, in the panel's block layout.
 *
 *   byte = (y / 2) * 75 + (x / 4)
 *   bit  = 7 - ((x % 4) * 2 + (y % 2))
 *
 * Computed inline rather than through a lookup table - it is four shifts, and
 * a table covering 120000 pixels would cost 360 kB of PSRAM to save nothing.
 */
static inline uint32_t st7305_byte_index(uint16_t x, uint16_t y)
{
    return (uint32_t)(y >> 1) * ST7305_STRIDE + (x >> 2);
}

static inline uint8_t st7305_bit_mask(uint16_t x, uint16_t y)
{
    return (uint8_t)(1u << (7u - (((x & 3u) << 1) | (y & 1u))));
}

static inline void st7305_set_pixel(uint8_t *fb, uint16_t x, uint16_t y,
                                    bool white)
{
    const uint32_t i = st7305_byte_index(x, y);
    const uint8_t  m = st7305_bit_mask(x, y);
    if (white) {
        fb[i] |= m;
    } else {
        fb[i] &= (uint8_t)~m;
    }
}

/* Push the whole framebuffer to the glass. */
esp_err_t st7305_refresh(st7305_t *dev);

/* High power mode is faster and darker; low power mode sips current. */
esp_err_t st7305_set_low_power(st7305_t *dev, bool low_power);
esp_err_t st7305_display_on(st7305_t *dev, bool on);

#endif /* ST7305_H */

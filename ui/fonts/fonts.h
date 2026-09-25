/*
 * The UI's fonts: X11 misc-fixed, the bitmap family behind u8g2's
 * u8g2_font_6x13_tf - the font Waveshare's own U8g2 example for this board
 * draws with. Regenerate with tools/make-fonts.sh; see that script for why a
 * font designed at one bit beats an outline font rasterised down to one.
 *
 * The whole ladder is one family, so the sizes sit together:
 *
 *   S   6x13   the workhorse - cell rows, captions, footers
 *   M   7x14   badges
 *   L   9x15   titles, field values, warnings
 *   XL  10x20  the big readouts
 *   XXL 6x13B doubled  the NO DATA alert - misc-fixed's Latin strikes
 *                      stop at 10x20, and its 12x24 is a different design
 *
 * Every size below XL has a real bold cut, which is what carries emphasis;
 * with Montserrat Medium there was no bold to reach for and warnings had to
 * grow a size instead.
 */
#ifndef UI_FONTS_H
#define UI_FONTS_H

#include "lvgl.h"

extern const lv_font_t fixed_6x13;
extern const lv_font_t fixed_6x13b;
extern const lv_font_t fixed_7x14;
extern const lv_font_t fixed_7x14b;
extern const lv_font_t fixed_9x15;
extern const lv_font_t fixed_9x15b;
extern const lv_font_t fixed_10x20;
extern const lv_font_t fixed_10x20b;
extern const lv_font_t fixed_6x13b_2x;

/*
 * The UI refers to fonts through these names, so swapping the whole set is one
 * edit here rather than a sweep through every page.
 */
#define UI_FONT_S    (&fixed_6x13)
#define UI_FONT_S_B  (&fixed_6x13b)
#define UI_FONT_M    (&fixed_7x14)
#define UI_FONT_M_B  (&fixed_7x14b)
#define UI_FONT_L    (&fixed_9x15)
#define UI_FONT_L_B  (&fixed_9x15b)
#define UI_FONT_XL   (&fixed_10x20b)
#define UI_FONT_XXL  (&fixed_6x13b_2x)

/*
 * Cell advance and line height of UI_FONT_S. The pages lay text out on a grid
 * rather than measuring it, which a monospaced font makes exact: a field n
 * characters wide is n * UI_FONT_S_W pixels, always.
 */
#define UI_FONT_S_W   6
#define UI_FONT_S_H  13

/* Cell advance of UI_FONT_XL. Emboldening widens the ink, not the advance. */
#define UI_FONT_XL_W 10

#endif /* UI_FONTS_H */

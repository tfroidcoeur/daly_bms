/*
 * The GUI, built on LVGL 9 and shared verbatim between the host simulator and
 * the ESP32 firmware.
 *
 * Design constraints, all set by the panel:
 *   - 300 x 400 portrait, one bit per pixel. No grey, no colour.
 *   - Reflective, slow to update. No animation; redraw only on change.
 *   - One usable button. Short press cycles pages, long press drills in.
 *
 * The UI never polls or decodes anything. It reads a system_model_t that the
 * poller owns and renders it.
 */
#ifndef UI_H
#define UI_H

#include <stddef.h>

#include "lvgl.h"

#include "fonts/fonts.h"

#include "bms_model.h"

/*
 * Screen geometry: 400 x 300 landscape.
 *
 * The glass scans natively as 300 x 400 portrait, so the device rotates in
 * flush_cb (see port/esp32/main/board.c). The simulator draws the landscape
 * buffer straight to the window - both show the same picture, which is what
 * you would see looking at the panel mounted on its side.
 */
#define UI_HOR_RES   400
#define UI_VER_RES   300
#define UI_VISIBLE_W 400

/*
 * Pages are inset by UI_PAGE_PAD on every side, so anything meant to span a
 * page - a rule, a full-width alarm band - is UI_CONTENT_W wide, not
 * UI_VISIBLE_W. Using the latter overflows the content box and is clipped at
 * the panel edge, taking any ellipsis with it.
 */
#define UI_PAGE_PAD   8
#define UI_CONTENT_W  (UI_VISIBLE_W - 2 * UI_PAGE_PAD)

/*
 * Full-scale current for the overview gauge, in amps. The dial reads magnitude
 * only, 0 to this value, so the whole sweep is available to the load instead of
 * half of it: a winch pull and a charge current of the same size land in the
 * same place. Direction is shown by the CHARGING legend inside the dial and by
 * the status line, not by which way the needle leans.
 *
 * Pick a full scale the winch actually reaches. At 1000 A a 45 A standing load
 * used 4 % of the sweep and the whole dial was dead space; the tick divisions
 * below assume this number divides evenly by five.
 */
#define UI_GAUGE_MAX_A 350

/*
 * Per-cell working range in millivolts: LiFePO4, empty to full. Both the cell
 * bars and the bank voltage column are scaled from these, so the two agree by
 * construction.
 */
#define UI_CELL_MIN_MV   2500
#define UI_CELL_MAX_MV   3650

/* Bank voltage column: the same range times the series count. */
#define UI_VBAR_CELLS      24
#define UI_VBAR_MIN_MV (UI_VBAR_CELLS * UI_CELL_MIN_MV)   /* 60.0 V */
#define UI_VBAR_MAX_MV (UI_VBAR_CELLS * UI_CELL_MAX_MV)   /* 87.6 V */

/*
 * LVGL renders anti-aliased RGB565; the panel is one bit. Both the device and
 * the simulator reduce a pixel with THIS function, so the simulator keeps
 * previewing exactly what the glass will show rather than a softer version.
 *
 * Rec. 601 luma, thresholded at mid-grey. Returns true for paper (lit), false
 * for ink.
 */
static inline bool ui_px_is_paper(uint16_t rgb565)
{
    const uint32_t r = (uint32_t)((rgb565 >> 11) & 0x1F) << 3;
    const uint32_t g = (uint32_t)((rgb565 >> 5) & 0x3F) << 2;
    const uint32_t b = (uint32_t)(rgb565 & 0x1F) << 3;
    return ((r * 77u + g * 151u + b * 28u) >> 8) >= 128u;
}

typedef enum {
    UI_KEY_SHORT,
    UI_KEY_LONG,
} ui_key_t;

typedef enum {
    UI_PAGE_OVERVIEW = 0,
    UI_PAGE_PACK1,
    UI_PAGE_PACK2,
    UI_PAGE_PACK3,
    UI_PAGE_COUNT,
} ui_page_t;

/* Build the widget tree. Call once, after lv_init() and the display exists. */
void ui_init(void);

/*
 * Push the current model into the widgets. Safe to call often; it compares
 * against what is already displayed and only touches widgets whose text
 * actually changes, which is what keeps a reflective panel from smearing.
 */
void ui_update(const system_model_t *model);

/* Feed a button event. */
void ui_input(ui_key_t key);

/* Which page is showing, and whether the cell detail overlay is open. */
ui_page_t ui_current_page(void);
bool      ui_cells_open(void);

/* ---- shared helpers, used by the page modules ---------------------------- */

/* Format fixed-point values into a caller-supplied buffer. */
void ui_fmt_volts(char *buf, size_t n, int32_t mv);      /* "53.13 V"  */
void ui_fmt_amps(char *buf, size_t n, int32_t ma);       /* "-44.1 A"  */
void ui_fmt_soc(char *buf, size_t n, uint16_t pct_x10);  /* "81.3 %"   */
void ui_fmt_watts(char *buf, size_t n, int32_t w);       /* "-2341 W"  */

/* A label styled for this panel: black ink, no anti-aliasing to smear. */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, const char *text);

/* A plain 1-bit progress bar: hollow rectangle with a solid fill.
 * LVGL infers orientation from the aspect ratio, so h > w fills upwards. */
lv_obj_t *ui_bar(lv_obj_t *parent, int32_t w, int32_t h);

/*
 * An arc gauge reading 0..`max`, with a filled band from zero to the value and
 * a needle at it. `needle_out` and `fill_out` receive the two moving parts;
 * pass all three to ui_gauge_set().
 */
lv_obj_t *ui_gauge(lv_obj_t *parent, int32_t size, int32_t max,
                   lv_obj_t **needle_out, lv_obj_t **fill_out);
void      ui_gauge_set(lv_obj_t *gauge, lv_obj_t *needle, lv_obj_t *fill,
                       int32_t value);

/* A caption above a value, the densest readable pairing on this panel.
 * Returns the value label; the caption is positioned 14 px above it. */
lv_obj_t *ui_field(lv_obj_t *parent, const char *caption, int32_t x, int32_t y);

/* Set a label's text only if it changed - every redraw is visible on glass. */
void ui_set_text(lv_obj_t *label, const char *text);

/*
 * How much a status line should shout.
 *
 * The panel has no colour, so emphasis is built from the two things a 1-bit
 * reflective display does have: a heavier cut of the same face, and inverting
 * the row. The bold cuts come from misc-fixed (UI_FONT_*_B); inversion is what
 * carries at a distance and survives being read at an angle in poor light.
 */
typedef enum {
    UI_EMPH_PLAIN,   /* routine text: small, ink on paper           */
    UI_EMPH_WARN,    /* larger, with a warning glyph                */
    UI_EMPH_ALARM,   /* larger, glyph, and the whole row inverted   */
    UI_EMPH_BADGE,   /* inverted, but sized to its text rather than the row */
} ui_emph_t;

void ui_set_emphasis(lv_obj_t *label, ui_emph_t emph);

/*
 * Compile-time assertion, C99-portable: an array cannot have negative size.
 *
 * The fonts are monospaced, so "does this string fit its column" is arithmetic
 * rather than a guess - and this is what turns that arithmetic into a build
 * error instead of a value quietly overlapping the next column.
 */
#define UI_STATIC_ASSERT(cond, tag) typedef char ui_assert_##tag[(cond) ? 1 : -1]

/* A full-width horizontal rule. */
lv_obj_t *ui_rule(lv_obj_t *parent);

/*
 * A label centred in a fixed-width column. The width is fixed rather than
 * content-sized so the text stays put as the value changes: "9.9 A" and
 * "-100.0 A" must not shuffle sideways under the gauge.
 */
lv_obj_t *ui_label_centered(lv_obj_t *parent, const lv_font_t *font,
                            const char *text, int32_t x, int32_t y, int32_t w);

/* Page builders. Each returns a screen-sized container, hidden until selected. */
lv_obj_t *ui_page_overview_create(lv_obj_t *parent);
void      ui_page_overview_update(const system_model_t *m);

lv_obj_t *ui_page_pack_create(lv_obj_t *parent, uint8_t slot);
void      ui_page_pack_update(uint8_t slot, const bms_pack_t *p);

lv_obj_t *ui_page_cells_create(lv_obj_t *parent);
void      ui_page_cells_update(const bms_pack_t *p);

#endif /* UI_H */

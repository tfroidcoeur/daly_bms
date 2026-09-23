#include "ui.h"

#include <stdio.h>
#include <string.h>

/* ---- navigation state ----------------------------------------------------- */

static ui_page_t g_page;
static bool      g_cells_open;

static lv_obj_t *g_pages[UI_PAGE_COUNT];
static lv_obj_t *g_cells_page;

/* ---- formatting ----------------------------------------------------------- */

void ui_fmt_volts(char *buf, size_t n, int32_t mv)
{
    const long whole = mv / 1000;
    const long frac  = ((mv < 0 ? -mv : mv) % 1000) / 10;
    /* Keep the sign visible even when the whole part rounds to zero. */
    const char *sign = (mv < 0 && whole == 0) ? "-" : "";
    snprintf(buf, n, "%s%ld.%02ld V", sign, whole, frac);
}

void ui_fmt_amps(char *buf, size_t n, int32_t ma)
{
    const long whole = ma / 1000;
    const long frac  = ((ma < 0 ? -ma : ma) % 1000) / 100;
    /* Keep the sign visible even when the whole part rounds to zero. */
    const char *sign = (ma < 0 && whole == 0) ? "-" : "";
    snprintf(buf, n, "%s%ld.%01ld A", sign, whole, frac);
}

void ui_fmt_soc(char *buf, size_t n, uint16_t pct_x10)
{
    snprintf(buf, n, "%u.%u %%", pct_x10 / 10, pct_x10 % 10);
}

void ui_fmt_watts(char *buf, size_t n, int32_t w)
{
    snprintf(buf, n, "%ld W", (long)w);
}

/* ---- widget helpers ------------------------------------------------------- */

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_bar(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 1000);   /* tenths of a percent */

    /* Hollow outline; a 1-bit panel has no grey to shade with. */
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    return bar;
}

/*
 * Arc gauge reading 0..max.
 *
 * 13 ticks with every third one major, so the labelled points fall on 0, a
 * quarter, a half, three quarters and full scale - enough to read the needle
 * against a scale without crowding a one-bit dial with numbers.
 */
lv_obj_t *ui_gauge(lv_obj_t *parent, int32_t size, int32_t max,
                   lv_obj_t **needle_out)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_set_size(scale, size, size);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(scale, 0, max);
    lv_scale_set_total_tick_count(scale, 13);
    lv_scale_set_major_tick_every(scale, 3);
    lv_scale_set_label_show(scale, true);
    lv_scale_set_angle_range(scale, 240);
    lv_scale_set_rotation(scale, 150);

    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(scale, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scale, 0, LV_PART_MAIN);

    /* Arc, minor ticks, major ticks: all solid black, varying only in weight. */
    lv_obj_set_style_arc_color(scale, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(scale, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(scale, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 9, LV_PART_INDICATOR);
    /* Major ticks and their labels share LV_PART_INDICATOR. */
    lv_obj_set_style_text_font(scale, UI_FONT_S, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(scale, 2, LV_PART_INDICATOR);

    lv_obj_t *needle = lv_line_create(scale);
    lv_obj_set_style_line_width(needle, 3, 0);
    lv_obj_set_style_line_color(needle, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(needle, false, 0);

    /* A hub, so the deliberately short needle reads as a needle rather than a
     * stray line. */
    lv_obj_t *hub = lv_obj_create(scale);
    lv_obj_set_size(hub, 9, 9);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);

    *needle_out = needle;
    return scale;
}

void ui_gauge_set(lv_obj_t *gauge, lv_obj_t *needle, int32_t value)
{
    /*
     * The needle stops at 55 % of the radius, well inside the ring of scale
     * labels. A full-length needle sits on top of whichever label it is nearest
     * - and that is exactly the number you are trying to read it against.
     */
    const int32_t r = lv_obj_get_width(gauge) / 2;
    lv_scale_set_line_needle_value(gauge, needle, r * 55 / 100, value);
}

lv_obj_t *ui_field(lv_obj_t *parent, const char *caption, int32_t x, int32_t y)
{
    lv_obj_t *cap = ui_label(parent, UI_FONT_S, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *val = ui_label(parent, UI_FONT_L, "-");
    lv_obj_align(val, LV_ALIGN_TOP_LEFT, x, y + 14);
    return val;
}

void ui_set_text(lv_obj_t *label, const char *text)
{
    const char *cur = lv_label_get_text(label);
    if (cur == NULL || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_set_emphasis(lv_obj_t *label, ui_emph_t emph)
{
    switch (emph) {
    case UI_EMPH_ALARM:
        /* Inverted: the row becomes a solid black band with white text. */
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, UI_CONTENT_W);
        lv_obj_set_style_text_font(label, UI_FONT_M_B, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(label, 3, 0);
        lv_obj_set_style_pad_ver(label, 1, 0);
        break;

    case UI_EMPH_BADGE:
        /* Same inversion as an alarm row, but hugging its text so it can sit
         * inside another element instead of spanning the page. */
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(label, UI_FONT_M_B, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(label, 6, 0);
        lv_obj_set_style_pad_ver(label, 2, 0);
        break;

    case UI_EMPH_WARN:
        /* Full width with dots, like an alarm row. Content-sized, an over-long
         * message would run off the panel with nothing to show it was cut; the
         * footer must stay exactly two rows however long a warning gets. */
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, UI_CONTENT_W);
        lv_obj_set_style_text_font(label, UI_FONT_M_B, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(label, 0, 0);
        lv_obj_set_style_pad_ver(label, 0, 0);
        break;

    case UI_EMPH_PLAIN:
    default:
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(label, UI_FONT_S, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(label, 0, 0);
        lv_obj_set_style_pad_ver(label, 0, 0);
        break;
    }
}

lv_obj_t *ui_label_centered(lv_obj_t *parent, const lv_font_t *font,
                            const char *text, int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *l = ui_label(parent, font, text);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

lv_obj_t *ui_rule(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, UI_CONTENT_W, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    return line;
}

/* ---- page container ------------------------------------------------------- */

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, UI_VISIBLE_W, UI_VER_RES);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, UI_PAGE_PAD, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

/* ---- lifecycle ------------------------------------------------------------ */

static void apply_visibility(void)
{
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        if (g_pages[i]) {
            lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_add_flag(g_cells_page, LV_OBJ_FLAG_HIDDEN);

    if (g_cells_open) {
        lv_obj_clear_flag(g_cells_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(g_pages[g_page], LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_pages[UI_PAGE_OVERVIEW] = ui_page_overview_create(make_page(scr));
    for (uint8_t s = 0; s < BMS_PACK_COUNT; s++) {
        g_pages[UI_PAGE_PACK1 + s] = ui_page_pack_create(make_page(scr), s);
    }
    g_cells_page = ui_page_cells_create(make_page(scr));

    g_page       = UI_PAGE_OVERVIEW;
    g_cells_open = false;
    apply_visibility();
}

void ui_input(ui_key_t key)
{
    if (key == UI_KEY_SHORT) {
        /* A short press always leaves the detail view and advances the page,
         * so a single button can never strand you somewhere. */
        if (g_cells_open) {
            g_cells_open = false;
        } else {
            g_page = (ui_page_t)((g_page + 1) % UI_PAGE_COUNT);
        }
    } else {
        /* Long press drills into cell detail, but only from a pack page -
         * there is no single pack to show from the overview. */
        if (g_page == UI_PAGE_OVERVIEW) {
            g_page = UI_PAGE_PACK1;
        } else {
            g_cells_open = !g_cells_open;
        }
    }
    apply_visibility();
}

void ui_update(const system_model_t *model)
{
    if (g_cells_open) {
        ui_page_cells_update(&model->pack[g_page - UI_PAGE_PACK1]);
        return;
    }
    if (g_page == UI_PAGE_OVERVIEW) {
        ui_page_overview_update(model);
    } else {
        const uint8_t slot = (uint8_t)(g_page - UI_PAGE_PACK1);
        ui_page_pack_update(slot, &model->pack[slot]);
    }
}

ui_page_t ui_current_page(void) { return g_page; }
bool      ui_cells_open(void)   { return g_cells_open; }

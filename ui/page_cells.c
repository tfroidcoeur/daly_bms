/*
 * Cell detail: every cell in one pack, 400 x 300 landscape.
 *
 * Two columns of twelve, so all twenty-four cells of the pack fit without
 * scrolling.
 *
 * Bars run on the absolute cell range, UI_CELL_MIN_MV to UI_CELL_MAX_MV
 * (2.50 V to 3.65 V), so a bar means state of charge and nothing else. Read
 * across the page and you are seeing how full the cells are, on the same scale
 * as the bank voltage column on the overview.
 *
 * The cost is that a healthy pack draws twenty-four near-identical bars: a
 * 15 mV spread is barely 1 % of the range. Finding the outlier is therefore the
 * job of the min/max arrows and the printed spread, not of bar length - which
 * is why those markers matter more here than they look.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

/*
 * Two columns of twelve, which covers a 24S pack without scrolling. Eight rows
 * per column fitted a 16S pack comfortably; twelve is the tighter arrangement
 * that keeps every cell of a 24S pack visible at once, which matters more than
 * the extra few pixels of row height.
 */
#define ROWS_PER_COL  12
#define COL_W        194
#define ROW_TOP       42
#define ROW_PITCH     21
#define MARK_X        28
#define BAR_X         46
#define BAR_W         96
#define VALUE_X      148

typedef struct {
    lv_obj_t *index;
    lv_obj_t *mark;   /* min / max / balancing marker */
    lv_obj_t *bar;
    lv_obj_t *mv;
} cell_row_t;

static lv_obj_t   *g_title;
static lv_obj_t   *g_sub;
static cell_row_t  g_rows[BMS_MAX_CELLS];
static uint8_t     g_rows_built;

lv_obj_t *ui_page_cells_create(lv_obj_t *page)
{
    g_title = ui_label(page, UI_FONT_L, "CELLS");
    lv_obj_align(g_title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* The nav hint moves up here so the bottom row can be given to a cell. */
    lv_obj_t *back = ui_label(page, UI_FONT_S, "KEY: back");
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 4);

    g_sub = ui_label(page, UI_FONT_S, "");
    lv_obj_align(g_sub, LV_ALIGN_TOP_LEFT, 0, 20);

    lv_obj_t *rule = ui_rule(page);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 36);

    /*
     * Build the maximum number of rows once and hide the unused ones. Creating
     * and destroying widgets as the cell count changes would churn the display.
     */
    g_rows_built = ROWS_PER_COL * 2;   /* 24, matching a 24S pack */
    for (uint8_t i = 0; i < g_rows_built; i++) {
        const int32_t col = i / ROWS_PER_COL;
        const int32_t x   = col * COL_W;
        const int32_t y   = ROW_TOP + (i % ROWS_PER_COL) * ROW_PITCH;
        cell_row_t *r = &g_rows[i];
        char idx[8];

        snprintf(idx, sizeof idx, "C%u", i + 1);
        r->index = ui_label(page, UI_FONT_S, idx);
        lv_obj_align(r->index, LV_ALIGN_TOP_LEFT, x, y + 2);

        r->mark = ui_label(page, UI_FONT_S, "");
        lv_obj_align(r->mark, LV_ALIGN_TOP_LEFT, x + MARK_X, y + 2);

        r->bar = ui_bar(page, BAR_W, 12);
        lv_obj_align(r->bar, LV_ALIGN_TOP_LEFT, x + BAR_X, y + 1);

        r->mv = ui_label(page, UI_FONT_S, "----");
        lv_obj_align(r->mv, LV_ALIGN_TOP_LEFT, x + VALUE_X, y + 2);
    }

    return page;
}

static void hide_row(cell_row_t *r)
{
    lv_obj_add_flag(r->index, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(r->mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(r->mv, LV_OBJ_FLAG_HIDDEN);
}

static void show_row(cell_row_t *r)
{
    lv_obj_clear_flag(r->index, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(r->mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(r->mv, LV_OBJ_FLAG_HIDDEN);
}

void ui_page_cells_update(const bms_pack_t *p)
{
    char buf[80];

    snprintf(buf, sizeof buf, "PACK %u CELLS", p->addr);
    ui_set_text(g_title, buf);

    if (!p->online || !p->cells_valid || p->cell_count == 0) {
        ui_set_text(g_sub, p->online ? "waiting for cell data" : "pack offline");
        for (uint8_t i = 0; i < g_rows_built; i++) {
            hide_row(&g_rows[i]);
        }
        return;
    }

    uint16_t lo = p->cell_mv[0], hi = p->cell_mv[0];
    for (uint8_t i = 1; i < p->cell_count; i++) {
        if (p->cell_mv[i] < lo) lo = p->cell_mv[i];
        if (p->cell_mv[i] > hi) hi = p->cell_mv[i];
    }

    bool any_balancing = false;
    for (uint8_t c = 0; c < p->cell_count; c++) {
        if ((p->balance_bits >> c) & 1u) {
            any_balancing = true;
            break;
        }
    }

    snprintf(buf, sizeof buf,
             "%u cells   bars %u-%u mV   now %u-%u   spread %u%s",
             p->cell_count, (unsigned)UI_CELL_MIN_MV, (unsigned)UI_CELL_MAX_MV,
             lo, hi, (unsigned)(hi - lo),
             any_balancing ? "   " LV_SYMBOL_REFRESH " bal" : "");
    ui_set_text(g_sub, buf);

    const uint8_t shown = p->cell_count < g_rows_built ? p->cell_count
                                                       : g_rows_built;
    for (uint8_t i = 0; i < g_rows_built; i++) {
        cell_row_t *r = &g_rows[i];
        if (i >= shown) {
            hide_row(r);
            continue;
        }
        show_row(r);

        const uint16_t mv = p->cell_mv[i];
        /* Absolute: empty cell = empty bar, full cell = full bar. Clamped so a
         * nonsense reading cannot draw outside the frame. */
        int32_t pct = ((int32_t)mv - UI_CELL_MIN_MV) * 1000 /
                      (UI_CELL_MAX_MV - UI_CELL_MIN_MV);
        if (pct < 0)    pct = 0;
        if (pct > 1000) pct = 1000;
        lv_bar_set_value(r->bar, pct, LV_ANIM_OFF);

        snprintf(buf, sizeof buf, "%u", mv);
        ui_set_text(r->mv, buf);

        /*
         * Arrows rather than the old "v" / "^" letters: they need no legend,
         * which is what let the legend row be reclaimed for a cell. Balancing
         * still needs naming, so the sub-heading does it - but only when some
         * cell is actually balancing.
         */
        const bool balancing = (p->balance_bits >> i) & 1u;
        if (mv == lo) {
            ui_set_text(r->mark, balancing ? LV_SYMBOL_DOWN LV_SYMBOL_REFRESH
                                           : LV_SYMBOL_DOWN);
        } else if (mv == hi) {
            ui_set_text(r->mark, balancing ? LV_SYMBOL_UP LV_SYMBOL_REFRESH
                                           : LV_SYMBOL_UP);
        } else if (balancing) {
            ui_set_text(r->mark, LV_SYMBOL_REFRESH);
        } else {
            ui_set_text(r->mark, "");
        }
    }

}

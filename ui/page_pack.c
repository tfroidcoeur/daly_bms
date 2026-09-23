/*
 * Pack detail: one BMS in full, 400 x 300 landscape.
 *
 * A tall SoC column down the left edge - the one number you read from across
 * the shed - with the supporting figures in two columns beside it.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define SOC_BAR_X    10
#define SOC_BAR_Y    34
#define SOC_BAR_W    46
#define SOC_BAR_H   210
#define COL1_X       72
#define COL2_X      232
#define FIELD_TOP    40
#define FIELD_PITCH  54

/*
 * Character budget per column, at UI_FONT_L's 9 px cell. The font is
 * monospaced, so this is exact rather than a guess: every value string below
 * is written to fit, and the two numbers are the only thing to re-check if a
 * column ever moves.
 */
#define COL1_CHARS ((COL2_X - COL1_X) / 9)                 /* 17 */
#define COL2_CHARS ((UI_CONTENT_W - COL2_X) / 9)           /* 16 */

/* The widest string each column can produce, at its worst-case values. */
UI_STATIC_ASSERT(sizeof "79.80 V  -100.0 A" - 1 <= COL1_CHARS, pack_col1);
UI_STATIC_ASSERT(sizeof "-25..-25 C  (16)" - 1 <= COL2_CHARS, pack_col2);

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *pct;
    lv_obj_t *va;
    lv_obj_t *watts;
    lv_obj_t *cells;
    lv_obj_t *spread;
    lv_obj_t *temps;
    lv_obj_t *mos;
    lv_obj_t *cycles;
    lv_obj_t *state;
    lv_obj_t *foot;
    lv_obj_t *nodata;
    lv_obj_t *body;   /* everything hidden together when the pack is offline */
} pack_view_t;

static pack_view_t g_view[BMS_PACK_COUNT];

lv_obj_t *ui_page_pack_create(lv_obj_t *page, uint8_t slot)
{
    pack_view_t *v = &g_view[slot];
    char title[24];

    snprintf(title, sizeof title, "PACK %u  (0x%02X)", slot + 1, slot + 1);
    lv_obj_t *t = ui_label(page, UI_FONT_L, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    v->nodata = ui_label(page, UI_FONT_XXL, "NO DATA");
    lv_obj_align(v->nodata, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(v->nodata, LV_OBJ_FLAG_HIDDEN);

    v->body = lv_obj_create(page);
    lv_obj_set_size(v->body, UI_CONTENT_W, UI_VER_RES - 26);
    lv_obj_align(v->body, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(v->body, lv_color_white(), 0);
    lv_obj_set_style_border_width(v->body, 0, 0);
    lv_obj_set_style_pad_all(v->body, 0, 0);
    lv_obj_clear_flag(v->body, LV_OBJ_FLAG_SCROLLABLE);

    v->bar = ui_bar(v->body, SOC_BAR_W, SOC_BAR_H);
    lv_obj_align(v->bar, LV_ALIGN_TOP_LEFT, SOC_BAR_X, SOC_BAR_Y - 22);
    v->pct = ui_label(v->body, UI_FONT_XL, "--.-%");
    lv_obj_align(v->pct, LV_ALIGN_TOP_LEFT, 0, SOC_BAR_Y + SOC_BAR_H - 14);

    v->va     = ui_field(v->body, "VOLTAGE / CURRENT", COL1_X, FIELD_TOP - 22);
    v->watts  = ui_field(v->body, "POWER",             COL1_X, FIELD_TOP - 22 + FIELD_PITCH);
    v->cells  = ui_field(v->body, "CELL MIN / MAX",    COL1_X, FIELD_TOP - 22 + FIELD_PITCH * 2);
    v->spread = ui_field(v->body, "SPREAD",            COL1_X, FIELD_TOP - 22 + FIELD_PITCH * 3);

    v->temps  = ui_field(v->body, "TEMPERATURE", COL2_X, FIELD_TOP - 22);
    v->mos    = ui_field(v->body, "MOSFETS",     COL2_X, FIELD_TOP - 22 + FIELD_PITCH);
    v->cycles = ui_field(v->body, "CYCLES",      COL2_X, FIELD_TOP - 22 + FIELD_PITCH * 2);
    v->state  = ui_field(v->body, "STATE",       COL2_X, FIELD_TOP - 22 + FIELD_PITCH * 3);

    /* Left-aligned to the field columns: starting at COL2_X ran the text off
     * the right edge of the panel. */
    v->foot = ui_label(v->body, UI_FONT_S, "");
    lv_obj_align(v->foot, LV_ALIGN_BOTTOM_LEFT, COL1_X, -4);

    return page;
}

void ui_page_pack_update(uint8_t slot, const bms_pack_t *p)
{
    pack_view_t *v = &g_view[slot];
    char buf[80], a[24], b[24];

    if (!p->online) {
        lv_obj_clear_flag(v->nodata, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->body, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(v->nodata, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->body, LV_OBJ_FLAG_HIDDEN);

    ui_fmt_soc(buf, sizeof buf, p->soc_pct_x10);
    ui_set_text(v->pct, buf);
    lv_bar_set_value(v->bar, p->soc_pct_x10, LV_ANIM_OFF);

    ui_fmt_volts(a, sizeof a, p->pack_mv);
    ui_fmt_amps(b, sizeof b, p->pack_ma);
    snprintf(buf, sizeof buf, "%s  %s", a, b);
    ui_set_text(v->va, buf);

    ui_fmt_watts(buf, sizeof buf, bms_pack_watts(p));
    ui_set_text(v->watts, buf);

    /* "3319#6 / 3334#11" - 16 of COL1_CHARS. The wider spacing this had under
     * a proportional font no longer fits. */
    snprintf(buf, sizeof buf, "%u#%u / %u#%u",
             p->cell_min_mv, p->cell_min_idx, p->cell_max_mv, p->cell_max_idx);
    ui_set_text(v->cells, buf);

    /* "24S" rather than "24 cells": the long form no longer fits COL1_CHARS,
     * and series count is how a pack is spoken about anyway. */
    snprintf(buf, sizeof buf, "%u mV over %uS",
             bms_pack_cell_delta_mv(p), p->cell_count);
    ui_set_text(v->spread, buf);

    snprintf(buf, sizeof buf, "%d..%d C  (%u)",
             p->temp_min_c, p->temp_max_c, p->temp_count);
    ui_set_text(v->temps, buf);

    snprintf(buf, sizeof buf, "chg %s  dsg %s",
             p->chg_mos ? "ON" : "off", p->dsg_mos ? "ON" : "off");
    ui_set_text(v->mos, buf);

    snprintf(buf, sizeof buf, "%u  %lu.%lu Ah", p->cycles,
             (unsigned long)(p->remaining_mah / 1000),
             (unsigned long)((p->remaining_mah % 1000) / 100));
    ui_set_text(v->cycles, buf);

    ui_set_text(v->state, p->charge_state == 1 ? "charging"
                        : (p->charge_state == 2 ? "discharging" : "idle"));

    if (p->alarm_active) {
        snprintf(buf, sizeof buf, "ALARM %02X %02X %02X %02X %02X %02X %02X",
                 p->alarms[0], p->alarms[1], p->alarms[2], p->alarms[3],
                 p->alarms[4], p->alarms[5], p->alarms[6]);
    } else {
        snprintf(buf, sizeof buf, "no alarms      hold KEY for cells");
    }
    ui_set_text(v->foot, buf);
}

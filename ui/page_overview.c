/*
 * Overview: the whole bank at a glance, 400 x 300 landscape.
 *
 * Almost all of the page is three instruments - a vertical SoC column, a
 * centre-zero current gauge, a vertical voltage column - which answer "how
 * full, which way, how hard" without reading a number.
 *
 * Per-pack figures deliberately do not appear here. They crowded the page, and
 * they are one button press away on the pack pages. What belongs on the front
 * screen is the bank as a whole, plus anything that needs a person.
 *
 * That last part is the two-line footer. Normally it carries the online count,
 * what the bank is doing (charging / discharging / idle, as the BMS units
 * themselves report it) and the button hint. When something is wrong, warnings take those
 * lines over, worst first - so an anomaly displaces the routine text rather
 * than being tucked in beside it, and the page never reflows.
 *
 * Alarms invert the whole row, white on black. There is no colour on this panel
 * and no bold weight in the built-in font, and an inverted band is louder than
 * either would have been.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "warnings.h"

/*
 * Instruments, laid out as three equal columns across the content width. Each
 * value is centred in its own column, directly under the thing it measures.
 *
 * No captions: "81.4 %", "-44.7 A" and "53.13 V" carry their own units, and on
 * a panel this size a row of headings was just noise.
 */
#define COL_W         128                    /* (400 - 2*8 padding) / 3 */
#define COL0_CX        64
#define COL1_CX       192
#define COL2_CX       320

#define INST_TOP       14
/*
 * Narrow bars. They carry one number each and read fine at this width, and the
 * space they give up goes to the dial, which is the instrument you watch during
 * a pull.
 */
#define BAR_W          30
#define BAR_H         170

/*
 * The two bars sit out at the edges of the glass rather than centred in their
 * thirds - they are the instruments you read from across a shed, and pushing
 * them apart leaves the dial room to breathe. This gap is measured from the
 * panel edge, so it has to lose the page's own padding.
 */
#define BAR_EDGE_GAP   20
#define BAR_INSET     (BAR_EDGE_GAP - UI_PAGE_PAD)
#define SOC_X          BAR_INSET
#define VOLT_X        (UI_CONTENT_W - BAR_INSET - BAR_W)

#define GAUGE_SIZE    210
#define GAUGE_X       (COL1_CX - GAUGE_SIZE / 2)
#define VALUE_Y       190
#define POWER_Y       214

/*
 * The outer readings are wider than the bars they belong to, so they cannot be
 * centred on them without running off the content edge. They pin to the edges
 * instead - a few pixels off the bar's centre line, and still unmistakably the
 * number for that bar.
 */
#define OUTER_VAL_W   (7 * UI_FONT_XL_W)
#define SOC_VAL_X     0
#define VOLT_VAL_X    (UI_CONTENT_W - OUTER_VAL_W)

UI_STATIC_ASSERT(sizeof "100.0 %" - 1 <= OUTER_VAL_W / UI_FONT_XL_W,
                 overview_soc_fits);
UI_STATIC_ASSERT(sizeof "87.60 V" - 1 <= OUTER_VAL_W / UI_FONT_XL_W,
                 overview_volt_fits);
/* The dial must still clear both bars. */
UI_STATIC_ASSERT(GAUGE_X > SOC_X + BAR_W, overview_gauge_clears_soc);
UI_STATIC_ASSERT(GAUGE_X + GAUGE_SIZE < VOLT_X, overview_gauge_clears_volt);

/*
 * Vertical placement of the dial.
 *
 * Centring the widget's box against the bars leaves the gauge looking too high,
 * because its ink is not centred in its box: the arc runs from lower-left to
 * lower-right through the top, so the highest ink is a full radius above the
 * centre while the lowest is only sin(30) - about half a radius - below it. The
 * visible centre therefore sits about r/4 (= GAUGE_SIZE/8) above the box
 * centre, and the widget has to come down by the same amount to look level.
 */
#define GAUGE_Y  (INST_TOP + BAR_H / 2 - GAUGE_SIZE / 2 + GAUGE_SIZE / 8)

/*
 * Direction legend, in the empty wedge at the bottom of the dial.
 *
 * Pinned up from the value row rather than down from the dial. Deriving it from
 * GAUGE_SIZE is what let a bigger dial walk the badge straight through the amp
 * value underneath it - the badge's job is to sit above that number, so that is
 * what it is measured from.
 *
 * Horizontally there is room whatever the radius: the arc ends, and with them
 * the needle's travel, stop 60 degrees short of straight down, so the wedge
 * below the hub is clear for a wide margin either side of the centre line.
 */
#define CHG_GAP  2
#define CHG_Y    (VALUE_Y - UI_BADGE_H - CHG_GAP)

/* The badge must clear the value row, and must still be inside the dial rather
 * than floating below it if the dial is ever resized again. */
UI_STATIC_ASSERT(CHG_Y + UI_BADGE_H <= VALUE_Y, overview_badge_clears_amps);
UI_STATIC_ASSERT(CHG_Y > GAUGE_Y + GAUGE_SIZE / 2, overview_badge_below_hub);
UI_STATIC_ASSERT(CHG_Y + UI_BADGE_H <= GAUGE_Y + GAUGE_SIZE,
                 overview_badge_inside_dial);

/* Two-line footer */
#define FOOT_RULE_Y   239
#define FOOT_LINE1_Y  246
#define FOOT_LINE2_Y  271
#define FOOT_LINES      2

static lv_obj_t *g_soc_bar, *g_soc_val;
static lv_obj_t *g_gauge, *g_needle, *g_fill, *g_amp_val, *g_watt_val, *g_chg_lbl;
static lv_obj_t *g_volt_bar, *g_volt_val;
static lv_obj_t *g_foot[FOOT_LINES];

lv_obj_t *ui_page_overview_create(lv_obj_t *page)
{
    g_soc_bar = ui_bar(page, BAR_W, BAR_H);        /* h > w = fills upward */
    lv_obj_align(g_soc_bar, LV_ALIGN_TOP_LEFT, SOC_X, INST_TOP);
    g_soc_val = ui_label_centered(page, UI_FONT_XL, "--.- %",
                                  SOC_VAL_X, VALUE_Y, OUTER_VAL_W);

    /* Centred on the same column, and optically levelled with the bars. */
    g_gauge = ui_gauge(page, GAUGE_SIZE, UI_GAUGE_MAX_A, &g_needle, &g_fill);
    lv_obj_align(g_gauge, LV_ALIGN_TOP_LEFT, GAUGE_X, GAUGE_Y);
    /*
     * The dial reads magnitude, so direction has to be said in words. Only
     * charging is called out: discharging is the resting state and the status
     * line names it anyway.
     *
     * Inverted, like an alarm row - it is a state change rather than a reading,
     * and it should catch the eye the same way. Aligned to the page centre,
     * which is the gauge column's centre, and sitting just above the amp value
     * in the dial's bottom wedge - see CHG_Y.
     */
    g_chg_lbl = ui_label(page, UI_FONT_M_B, "CHARGING");
    ui_set_emphasis(g_chg_lbl, UI_EMPH_BADGE);
    lv_obj_align(g_chg_lbl, LV_ALIGN_TOP_MID, 0, CHG_Y);
    lv_obj_add_flag(g_chg_lbl, LV_OBJ_FLAG_HIDDEN);

    g_amp_val = ui_label_centered(page, UI_FONT_XL, "--.- A",
                                  COL1_CX - COL_W / 2, VALUE_Y, COL_W);
    /* Power belongs with the current it is derived from, not off in the status
     * bar - reading amps and watts together is how you judge a load. */
    g_watt_val = ui_label_centered(page, UI_FONT_L_B, "-- W",
                                   COL1_CX - COL_W / 2, POWER_Y, COL_W);

    g_volt_bar = ui_bar(page, BAR_W, BAR_H);
    lv_obj_align(g_volt_bar, LV_ALIGN_TOP_LEFT, VOLT_X, INST_TOP);
    g_volt_val = ui_label_centered(page, UI_FONT_XL, "--.-- V",
                                   VOLT_VAL_X, VALUE_Y, OUTER_VAL_W);

    lv_obj_t *rule = ui_rule(page);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, FOOT_RULE_Y);

    g_foot[0] = ui_label(page, UI_FONT_S_B, "");
    lv_obj_align(g_foot[0], LV_ALIGN_TOP_LEFT, 0, FOOT_LINE1_Y);
    g_foot[1] = ui_label(page, UI_FONT_S_B, "");
    lv_obj_align(g_foot[1], LV_ALIGN_TOP_LEFT, 0, FOOT_LINE2_Y);
    for (int i = 0; i < FOOT_LINES; i++) {
        ui_set_emphasis(g_foot[i], UI_EMPH_PLAIN);
    }

    return page;
}

/* Map a bank voltage onto the bar's 0..1000 range, clamped at both ends. */
static int32_t volts_to_bar(int32_t mv)
{
    if (mv <= UI_VBAR_MIN_MV) {
        return 0;
    }
    if (mv >= UI_VBAR_MAX_MV) {
        return 1000;
    }
    return (mv - UI_VBAR_MIN_MV) * 1000 / (UI_VBAR_MAX_MV - UI_VBAR_MIN_MV);
}

void ui_page_overview_update(const system_model_t *m)
{
    const bms_summary_t s = bms_model_summary(m);
    warning_set_t warn;
    char buf[96];

    /* --- instruments ----------------------------------------------------- */

    if (s.online_count == 0) {
        ui_set_text(g_soc_val, "--.- %");
        ui_set_text(g_amp_val, "--.- A");
        ui_set_text(g_volt_val, "--.-- V");
        ui_set_text(g_watt_val, "-- W");
        lv_obj_add_flag(g_chg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(g_soc_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(g_volt_bar, 0, LV_ANIM_OFF);
        ui_gauge_set(g_gauge, g_needle, g_fill, 0);
    } else {
        ui_fmt_soc(buf, sizeof buf, s.mean_soc_pct_x10);
        ui_set_text(g_soc_val, buf);
        lv_bar_set_value(g_soc_bar, s.mean_soc_pct_x10, LV_ANIM_OFF);

        /* Magnitude everywhere in this column - the dial cannot show a sign,
         * so a signed number underneath it would contradict the needle. */
        const int32_t abs_ma = s.total_ma < 0 ? -s.total_ma : s.total_ma;
        const int32_t watts  = bms_summary_watts(&s);
        const int32_t abs_w  = watts < 0 ? -watts : watts;

        ui_fmt_amps(buf, sizeof buf, abs_ma);
        ui_set_text(g_amp_val, buf);
        ui_gauge_set(g_gauge, g_needle, g_fill, abs_ma / 1000);

        ui_fmt_watts(buf, sizeof buf, abs_w);
        ui_set_text(g_watt_val, buf);

        if (s.charge_state == 1) {
            lv_obj_clear_flag(g_chg_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_chg_lbl, LV_OBJ_FLAG_HIDDEN);
        }

        ui_fmt_volts(buf, sizeof buf, s.bank_mv);
        ui_set_text(g_volt_val, buf);
        lv_bar_set_value(g_volt_bar, volts_to_bar(s.bank_mv), LV_ANIM_OFF);
    }

    /* --- footer: warnings first, routine text with whatever is left ------- */

    warnings_evaluate(m, &warn);
    const uint8_t shown = warn.count < FOOT_LINES ? warn.count : FOOT_LINES;

    for (uint8_t i = 0; i < FOOT_LINES; i++) {
        if (i < shown) {
            const bool alarm = warn.item[i].level == WARN_LEVEL_ALARM;
            char line[64];

            warnings_format(&warn.item[i], line, sizeof line);

            /*
             * Anything that did not fit is counted on the last warning line;
             * silently dropping the rest would make two warnings and twelve
             * look identical.
             */
            if (i == shown - 1 && warn.count > shown) {
                snprintf(buf, sizeof buf, LV_SYMBOL_WARNING " %s  (+%u more%s)",
                         line, (unsigned)(warn.count - shown),
                         warn.truncated ? "+" : "");
            } else {
                snprintf(buf, sizeof buf, LV_SYMBOL_WARNING " %s", line);
            }
            ui_set_emphasis(g_foot[i], alarm ? UI_EMPH_ALARM : UI_EMPH_WARN);
            ui_set_text(g_foot[i], buf);
            continue;
        }

        /* Routine text fills whatever lines the warnings did not take. */
        ui_set_emphasis(g_foot[i], UI_EMPH_PLAIN);
        if (i == 0) {
            /* What the bank is doing matters more here than any number the
             * instruments already show. */
            const char *state = s.online_count == 0 ? "no data"
                              : (s.charge_state == 1 ? "CHARGING"
                              : (s.charge_state == 2 ? "discharging" : "idle"));
            snprintf(buf, sizeof buf, "%u/%u online      %s%s",
                     s.online_count, BMS_PACK_COUNT, state,
                     (s.charger_present && s.charge_state != 1)
                         ? "   charger connected" : "");
        } else {
            /* Nothing to say. The controls are discovered by touching them,
               and a static hint costs a line that a warning may need. */
            buf[0] = '\0';
        }
        ui_set_text(g_foot[i], buf);
    }
}

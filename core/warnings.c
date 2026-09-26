#include "warnings.h"

#include <stdio.h>
#include <string.h>

/* Severity first, then pack, then code: the display order, and the order the
 * set is trimmed to when more warnings are found than will fit. */
static bool outranks(const warning_t *a, const warning_t *b)
{
    if (a->level != b->level) return a->level > b->level;
    if (a->pack  != b->pack)  return a->pack  < b->pack;
    return a->code < b->code;
}

static void push(warning_set_t *s, warn_code_t code, warn_level_t level,
                 uint8_t pack, int32_t a, int32_t b)
{
    const warning_t w = { code, level, pack, a, b };

    if (s->count < WARN_MAX) {
        s->item[s->count++] = w;
        return;
    }

    /*
     * Full. Dropping the newcomer would make what survives depend on the order
     * the rules happen to run in - and the bank-level rules run last, so a
     * blown fuse would lose its place to three packs' worth of cell drift.
     * Evict the weakest line instead, and only if this one beats it.
     */
    s->truncated = true;

    uint8_t weakest = 0;
    for (uint8_t i = 1; i < s->count; i++) {
        if (outranks(&s->item[weakest], &s->item[i])) {
            weakest = i;
        }
    }
    if (outranks(&w, &s->item[weakest])) {
        s->item[weakest] = w;
    }
}

/* Alarms first, then by pack, then by code - stable and predictable. */
static void sort_by_severity(warning_set_t *s)
{
    for (uint8_t i = 1; i < s->count; i++) {
        const warning_t key = s->item[i];
        int j = (int)i - 1;
        while (j >= 0) {
            const warning_t *c = &s->item[j];
            if (!outranks(&key, c)) {
                break;
            }
            s->item[j + 1] = *c;
            j--;
        }
        s->item[j + 1] = key;
    }
}

void warnings_evaluate(const system_model_t *m, warning_set_t *out)
{
    memset(out, 0, sizeof *out);

    /* Bank-wide checks need the online packs first. */
    int32_t  min_mv = 0, max_mv = 0;
    uint8_t  online = 0;
    uint8_t  ref_cells = 0;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            push(out, WARN_PACK_OFFLINE, WARN_LEVEL_ALARM, p->addr, 0, 0);
            continue;
        }
        if (online == 0) {
            min_mv = max_mv = p->pack_mv;
        } else {
            if (p->pack_mv < min_mv) min_mv = p->pack_mv;
            if (p->pack_mv > max_mv) max_mv = p->pack_mv;
        }
        online++;

        /* The reference is the first pack that has actually reported a
         * geometry, not merely the first that is online. */
        if (ref_cells == 0) {
            ref_cells = p->cell_count;
        }

        /* Cell count should match across identical packs. Only meaningful once
         * a pack has actually reported its geometry. */
        if (ref_cells && p->cell_count && p->cell_count != ref_cells) {
            push(out, WARN_CELL_COUNT_MISMATCH, WARN_LEVEL_ALARM, p->addr,
                 p->cell_count, ref_cells);
        }

        /* A reading outside the sane band is a broken measurement, not a low
         * cell - report the first offending cell rather than all of them. */
        if (p->cells_valid) {
            for (uint8_t c = 0; c < p->cell_count; c++) {
                const uint16_t mv = p->cell_mv[c];
                if (mv < WARN_CELL_MIN_SANE_MV || mv > WARN_CELL_MAX_SANE_MV) {
                    push(out, WARN_CELL_IMPLAUSIBLE, WARN_LEVEL_ALARM, p->addr,
                         c + 1, mv);
                    break;
                }
            }
        }

        const uint16_t spread = bms_pack_cell_delta_mv(p);
        if (spread >= WARN_CELL_SPREAD_ALARM_MV) {
            push(out, WARN_CELL_SPREAD, WARN_LEVEL_ALARM, p->addr, spread, 0);
        } else if (spread >= WARN_CELL_SPREAD_MV) {
            push(out, WARN_CELL_SPREAD, WARN_LEVEL_WARN, p->addr, spread, 0);
        }

        if (p->alarm_active) {
            /* Report the first flag byte that is set; the pack page shows all
             * seven. We do not name specific faults until the bit meanings have
             * been confirmed against real hardware. */
            for (uint8_t b = 0; b < BMS_ALARM_BYTES; b++) {
                if (p->alarms[b]) {
                    push(out, WARN_BMS_FAULT, WARN_LEVEL_ALARM, p->addr,
                         b, p->alarms[b]);
                    break;
                }
            }
        }

        if (p->soc_valid && p->soc_pct_x10 <= WARN_SOC_CRIT_PCT_X10) {
            push(out, WARN_SOC_LOW, WARN_LEVEL_ALARM, p->addr,
                 p->soc_pct_x10, 0);
        } else if (p->soc_valid && p->soc_pct_x10 <= WARN_SOC_LOW_PCT_X10) {
            push(out, WARN_SOC_LOW, WARN_LEVEL_WARN, p->addr,
                 p->soc_pct_x10, 0);
        }

        if (p->temp_minmax_valid && p->temp_max_c >= WARN_TEMP_HIGH_ALARM_C) {
            push(out, WARN_TEMP_HIGH, WARN_LEVEL_ALARM, p->addr,
                 p->temp_max_c, 0);
        } else if (p->temp_minmax_valid && p->temp_max_c >= WARN_TEMP_HIGH_C) {
            push(out, WARN_TEMP_HIGH, WARN_LEVEL_WARN, p->addr,
                 p->temp_max_c, 0);
        }

        /*
         * Charging a lithium-iron cell below freezing plates lithium and does
         * permanent damage, so that is an alarm regardless of how cold it is.
         * Merely being cold is a warning.
         *
         * "Charging" is the BMS's own state from 0x93, never the sign of the
         * current: the Daly drivers in circulation disagree about which way
         * that points, and a rule keyed on the sign fails in exactly the
         * dangerous direction - silent while charging - on a pack that runs the
         * other way. A trickle below the BMS's own charging threshold reads as
         * idle here; its built-in charge-temperature protection, which 0x98
         * reports by name, covers that end.
         */
        if (!p->temp_minmax_valid) {
            /* nothing to judge yet */
        } else if (p->temp_min_c < 0 && p->charge_state == 1) {
            push(out, WARN_CHARGING_BELOW_ZERO, WARN_LEVEL_ALARM, p->addr,
                 p->temp_min_c, 0);
        } else if (p->temp_min_c <= WARN_TEMP_LOW_ALARM_C) {
            push(out, WARN_TEMP_LOW, WARN_LEVEL_ALARM, p->addr,
                 p->temp_min_c, 0);
        } else if (p->temp_min_c <= WARN_TEMP_LOW_C) {
            push(out, WARN_TEMP_LOW, WARN_LEVEL_WARN, p->addr,
                 p->temp_min_c, 0);
        }
    }

    /*
     * Packs in parallel are electrically tied together, so their voltages can
     * only differ if something is between them: an open fuse, a lifted joint,
     * a contactor that has not closed. That makes this a bank-level alarm, not
     * a per-pack one.
     */
    if (online >= 2 && (max_mv - min_mv) >= WARN_PACK_MV_MISMATCH) {
        push(out, WARN_PACK_VOLTAGE_MISMATCH, WARN_LEVEL_ALARM, 0,
             max_mv - min_mv, 0);
    }

    sort_by_severity(out);
}

bool warnings_any_alarm(const warning_set_t *s)
{
    for (uint8_t i = 0; i < s->count; i++) {
        if (s->item[i].level == WARN_LEVEL_ALARM) {
            return true;
        }
    }
    return false;
}

void warnings_format(const warning_t *w, char *buf, size_t n)
{
    char who[12];

    if (w->pack == 0) {
        snprintf(who, sizeof who, "BANK");
    } else {
        snprintf(who, sizeof who, "PACK %u", w->pack);
    }

    switch (w->code) {
    case WARN_PACK_OFFLINE:
        snprintf(buf, n, "%s  not responding", who);
        break;
    case WARN_PACK_VOLTAGE_MISMATCH:
        snprintf(buf, n, "%s  packs differ %ld.%02ld V, check joints",
                 who, (long)(w->a / 1000), (long)(w->a % 1000) / 10);
        break;
    case WARN_CELL_COUNT_MISMATCH:
        snprintf(buf, n, "%s  %ld cells, expected %ld",
                 who, (long)w->a, (long)w->b);
        break;
    case WARN_CELL_IMPLAUSIBLE:
        snprintf(buf, n, "%s  cell %ld reads %ld mV, sense?",
                 who, (long)w->a, (long)w->b);
        break;
    case WARN_CELL_SPREAD:
        snprintf(buf, n, "%s  cell spread %ld mV", who, (long)w->a);
        break;
    case WARN_BMS_FAULT:
        snprintf(buf, n, "%s  BMS fault flags [%ld]=0x%02lX",
                 who, (long)w->a, (long)w->b);
        break;
    case WARN_SOC_LOW:
        snprintf(buf, n, "%s  SoC %ld.%ld %% low",
                 who, (long)w->a / 10, (long)w->a % 10);
        break;
    case WARN_TEMP_HIGH:
        snprintf(buf, n, "%s  %ld C too hot", who, (long)w->a);
        break;
    case WARN_TEMP_LOW:
        snprintf(buf, n, "%s  %ld C too cold", who, (long)w->a);
        break;
    case WARN_CHARGING_BELOW_ZERO:
        snprintf(buf, n, "%s  charging at %ld C - stop", who, (long)w->a);
        break;
    default:
        snprintf(buf, n, "%s  unknown condition", who);
        break;
    }
}

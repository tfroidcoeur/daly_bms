#include "virtual_bms.h"

#include <string.h>

void vbms_init(vbms_t *v, bool enabled)
{
    memset(v, 0, sizeof *v);
    v->enabled = enabled;
}

/* --- aggregation ---------------------------------------------------------- */

static void aggregate_soc(const system_model_t *m, bms_pack_t *out)
{
    int64_t mv = 0, soc = 0;
    int32_t ma = 0;
    uint8_t n = 0;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online || !p->soc_valid) {
            continue;
        }
        mv  += p->pack_mv;
        ma  += p->pack_ma;
        soc += p->soc_pct_x10;
        n++;
    }
    if (n) {
        out->pack_mv     = (int32_t)(mv / n);
        out->pack_ma     = ma;
        out->soc_pct_x10 = (uint16_t)(soc / n);
        out->soc_valid   = true;
    }
}

static void aggregate_minmax(const system_model_t *m, bms_pack_t *out,
                             const uint8_t temp_offset[BMS_PACK_COUNT])
{
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            continue;
        }
        /* 0x91 has no flag of its own; a zero max means it has not landed. */
        if (p->cell_max_mv) {
            if (p->cell_max_mv > out->cell_max_mv) {
                out->cell_max_mv  = p->cell_max_mv;
                out->cell_max_idx = p->cell_max_idx;
            }
            if (!out->cell_min_mv || p->cell_min_mv < out->cell_min_mv) {
                out->cell_min_mv  = p->cell_min_mv;
                out->cell_min_idx = p->cell_min_idx;
            }
        }
        /* Sensor numbers are renumbered into the concatenated list, so they
         * still point at the right entry of 0x96. */
        if (p->temp_minmax_valid) {
            if (!out->temp_minmax_valid || p->temp_max_c > out->temp_max_c) {
                out->temp_max_c   = p->temp_max_c;
                out->temp_max_idx = (uint8_t)(temp_offset[i] + p->temp_max_idx);
            }
            if (!out->temp_minmax_valid || p->temp_min_c < out->temp_min_c) {
                out->temp_min_c   = p->temp_min_c;
                out->temp_min_idx = (uint8_t)(temp_offset[i] + p->temp_min_idx);
            }
            out->temp_minmax_valid = true;
        }
    }
}

/*
 * The cell array. The packs are separate series strings, so cell 5 of one pack
 * and cell 5 of another are not the same node and their mean describes nothing
 * physical - worse, it hides exactly the weak cell a reader is looking for.
 * So each position shows whichever pack's cell there strays furthest from the
 * bank mean. A reader that works out min, max or spread from the array (and
 * many do, ignoring 0x91) then sees the bank's real extremes, unless the
 * highest and lowest cells share a position in different packs; 0x91 still
 * carries both.
 *
 * Only packs agreeing with the first one's cell count take part - a mismatch
 * is already a warning on our own screen, and mixing geometries would put
 * cells in positions that do not exist.
 */
static void aggregate_cells(const system_model_t *m, bms_pack_t *out)
{
    const bms_pack_t *use[BMS_PACK_COUNT];
    uint8_t n = 0;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online || p->cell_count == 0) {
            continue;
        }
        if (out->cell_count == 0) {
            out->cell_count = p->cell_count;
        }
        if (p->cell_count == out->cell_count && p->cells_valid) {
            use[n++] = p;
        }
    }
    if (n == 0) {
        return;
    }

    int64_t sum = 0;
    for (uint8_t k = 0; k < n; k++) {
        for (uint8_t c = 0; c < out->cell_count; c++) {
            sum += use[k]->cell_mv[c];
        }
    }
    const int32_t mean = (int32_t)(sum / ((int64_t)n * out->cell_count));

    for (uint8_t c = 0; c < out->cell_count; c++) {
        int32_t best = -1;
        for (uint8_t k = 0; k < n; k++) {
            int32_t dev = (int32_t)use[k]->cell_mv[c] - mean;
            if (dev < 0) {
                dev = -dev;
            }
            if (dev > best) {
                best = dev;
                out->cell_mv[c] = use[k]->cell_mv[c];
            }
        }
    }
    out->cells_valid = true;

    for (uint8_t k = 0; k < n; k++) {
        out->balance_bits |= use[k]->balance_bits;
    }
}

/*
 * Every sensor of every pack that has said how many it has, pack 1 first, as
 * long as they fit in Daly's 16. A pack that does not fit is left out whole
 * rather than cut off part way. The set is published only once every pack in
 * it has delivered a full one, so 0x94's count and 0x96's contents agree.
 */
static void aggregate_temps(const system_model_t *m, bms_pack_t *out,
                            uint8_t temp_offset[BMS_PACK_COUNT])
{
    bool all_valid = true;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        temp_offset[i] = 0;
        if (!p->online || p->temp_count == 0 ||
            out->temp_count + p->temp_count > BMS_MAX_TEMPS) {
            continue;
        }
        temp_offset[i] = out->temp_count;
        memcpy(&out->temp_c[out->temp_count], p->temp_c, p->temp_count);
        out->temp_count = (uint8_t)(out->temp_count + p->temp_count);
        all_valid = all_valid && p->temps_valid;
    }
    out->temps_valid = out->temp_count > 0 && all_valid;
}

void vbms_aggregate(const system_model_t *m, bms_pack_t *out)
{
    uint8_t temp_offset[BMS_PACK_COUNT];
    uint8_t online = 0;

    memset(out, 0, sizeof *out);
    aggregate_soc(m, out);
    aggregate_cells(m, out);
    aggregate_temps(m, out, temp_offset);
    aggregate_minmax(m, out, temp_offset);

    out->chg_mos = true;
    out->dsg_mos = true;

    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            continue;
        }
        online++;

        /* Same rule as the overview: charging wins over discharging. */
        if (p->charge_state == 1) {
            out->charge_state = 1;
        } else if (p->charge_state == 2 && out->charge_state == 0) {
            out->charge_state = 2;
        }
        /* A pack with a MOSFET open is not taking its share, and the reader
         * should hear about it rather than see the other two vouch for it. */
        out->chg_mos = out->chg_mos && p->chg_mos;
        out->dsg_mos = out->dsg_mos && p->dsg_mos;
        out->remaining_mah += p->remaining_mah;

        out->charger_present = out->charger_present || p->charger_present;
        out->load_present    = out->load_present || p->load_present;
        if (p->cycles_valid && (!out->cycles_valid || p->cycles > out->cycles)) {
            out->cycles       = p->cycles;
            out->cycles_valid = true;
        }

        for (uint8_t b = 0; b < BMS_ALARM_BYTES; b++) {
            out->alarms[b] |= p->alarms[b];
        }
    }

    out->online = online > 0;
    if (!out->online) {
        out->chg_mos = out->dsg_mos = false;
    }
    /* A missing pack leaves the bank short of a third of its capacity and
     * current rating, and nothing else in the payloads would say so. */
    if (online < BMS_PACK_COUNT) {
        out->alarms[VBMS_COMM_FAULT_BYTE] |= (uint8_t)(1u << VBMS_COMM_FAULT_BIT);
    }
    for (uint8_t b = 0; b < BMS_ALARM_BYTES; b++) {
        out->alarm_active = out->alarm_active || out->alarms[b] != 0;
    }
}

/* --- Victron encoding ---------------------------------------------------- */

/* Victron payloads are little-endian, unlike Daly's. */
static void put16le(uint8_t *d, int32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)((uint32_t)v >> 8);
}

static int32_t clamp_s16(int32_t v)
{
    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
}

/* Integer division rounding half away from zero. */
static int32_t div_round(int32_t n, int32_t d)
{
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

uint8_t vbms_encode(const bms_pack_t *a, can_frame_out_t out[VBMS_MAX_FRAMES])
{
    if (!a->online || !a->soc_valid) {
        return 0;
    }

    can_frame_out_t *f = &out[0];
    memset(f, 0, sizeof *f);
    f->id  = VBMS_ID_SOC;
    f->len = 6;
    put16le(&f->data[0], div_round(a->soc_pct_x10, 10));
    put16le(&f->data[2], VBMS_SOH_PCT);
    put16le(&f->data[4], (int32_t)a->soc_pct_x10 * 10);

    f = &out[1];
    memset(f, 0, sizeof *f);
    f->id  = VBMS_ID_VIT;
    f->len = 4;
    put16le(&f->data[0], clamp_s16(div_round(a->pack_mv, 10)));
    put16le(&f->data[2], clamp_s16(div_round(a->pack_ma, 100)));
    /* Temperature is optional: without one the frame is shorter rather than
     * claiming 0 C. */
    if (a->temp_minmax_valid) {
        put16le(&f->data[4], (int32_t)a->temp_max_c * 10);
        f->len = 6;
    }
    return 2;
}

uint8_t vbms_tick(vbms_t *v, const system_model_t *m, uint32_t now_ms,
                  can_frame_out_t out[VBMS_MAX_FRAMES])
{
    if (!v->enabled ||
        (v->started && (uint32_t)(now_ms - v->last_tx_ms) < VBMS_PERIOD_MS)) {
        return 0;
    }

    bms_pack_t agg;
    vbms_aggregate(m, &agg);
    const uint8_t n = vbms_encode(&agg, out);
    if (n == 0) {
        /* Nothing to say yet: the first frames go out as soon as there is,
         * not up to a period later. */
        return 0;
    }
    v->started    = true;
    v->last_tx_ms = now_ms;
    v->sent++;
    return n;
}

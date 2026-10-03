#include "virtual_bms.h"

#include <string.h>

#include "daly_proto.h"

bool vbms_init(vbms_t *v, uint8_t addr)
{
    memset(v, 0, sizeof *v);
    if (addr == 0 || addr == DALY_HOST_ADDR) {
        return false;
    }
    for (uint8_t i = 0; i < BMS_PACK_COUNT; i++) {
        if (addr == i + 1) {    /* bms_model_init's 0x01..0x03 */
            return false;
        }
    }
    v->addr = addr;
    return true;
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

/* --- encoding ------------------------------------------------------------- */

static void put16(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
}

static void put32(uint8_t *d, uint32_t v)
{
    put16(d, v >> 16);
    put16(d + 2, v);
}

/*
 * One frame per call, numbered from 1 as the firmware in service does it, so a
 * reader built against real packs needs no special case for this one.
 */
static uint8_t encode(const bms_pack_t *a, uint8_t cmd, uint8_t life,
                      uint8_t frames[VBMS_MAX_FRAMES][8])
{
    uint8_t *d = frames[0];
    memset(frames, 0, (size_t)VBMS_MAX_FRAMES * 8);

    switch (cmd) {
    case DALY_CMD_SOC: {
        if (!a->soc_valid) {
            return 0;
        }
        const uint32_t dv = (uint32_t)((a->pack_mv + 50) / 100);   /* 0.1 V */
        put16(&d[0], dv);
        put16(&d[2], dv);
        put16(&d[4], daly_encode_current_raw(a->pack_ma));
        put16(&d[6], a->soc_pct_x10);
        return 1;
    }

    case DALY_CMD_CELL_MINMAX:
        if (!a->cell_max_mv) {
            return 0;
        }
        put16(&d[0], a->cell_max_mv);
        d[2] = a->cell_max_idx;
        put16(&d[3], a->cell_min_mv);
        d[5] = a->cell_min_idx;
        return 1;

    case DALY_CMD_TEMP_MINMAX:
        if (!a->temp_minmax_valid) {
            return 0;
        }
        d[0] = daly_encode_temp_raw(a->temp_max_c);
        d[1] = a->temp_max_idx;
        d[2] = daly_encode_temp_raw(a->temp_min_c);
        d[3] = a->temp_min_idx;
        return 1;

    case DALY_CMD_MOS:
        d[0] = a->charge_state;
        d[1] = a->chg_mos;
        d[2] = a->dsg_mos;
        d[3] = life;
        put32(&d[4], a->remaining_mah);
        return 1;

    case DALY_CMD_STATUS:
        /* A reader rejects a zero count, and needs both to size 0x95/0x96. */
        if (!a->cell_count || !a->temp_count) {
            return 0;
        }
        d[0] = a->cell_count;
        d[1] = a->temp_count;
        d[2] = a->charger_present;
        d[3] = a->load_present;
        if (a->cycles_valid) {
            put16(&d[5], a->cycles);
        }
        return 1;

    case DALY_CMD_CELL_VOLTS: {
        if (!a->cells_valid) {
            return 0;
        }
        const uint8_t n = (uint8_t)((a->cell_count + DALY_CELLS_PER_FRAME - 1) /
                                    DALY_CELLS_PER_FRAME);
        for (uint8_t f = 0; f < n; f++) {
            frames[f][0] = (uint8_t)(f + 1);
            for (uint8_t s = 0; s < DALY_CELLS_PER_FRAME; s++) {
                const uint16_t c = (uint16_t)(f * DALY_CELLS_PER_FRAME + s);
                if (c < a->cell_count) {
                    put16(&frames[f][1 + s * 2], a->cell_mv[c]);
                }
            }
        }
        return n;
    }

    case DALY_CMD_CELL_TEMPS: {
        if (!a->temps_valid) {
            return 0;
        }
        const uint8_t n = (uint8_t)((a->temp_count + DALY_TEMPS_PER_FRAME - 1) /
                                    DALY_TEMPS_PER_FRAME);
        for (uint8_t f = 0; f < n; f++) {
            frames[f][0] = (uint8_t)(f + 1);
            for (uint8_t s = 0; s < DALY_TEMPS_PER_FRAME; s++) {
                const uint16_t t = (uint16_t)(f * DALY_TEMPS_PER_FRAME + s);
                if (t < a->temp_count) {
                    frames[f][1 + s] = daly_encode_temp_raw(a->temp_c[t]);
                }
            }
        }
        return n;
    }

    case DALY_CMD_BALANCE:
        if (!a->cells_valid) {
            return 0;
        }
        for (uint8_t i = 0; i < 8; i++) {
            d[i] = (uint8_t)(a->balance_bits >> (8 * i));
        }
        return 1;

    case DALY_CMD_FAULTS:
        memcpy(d, a->alarms, BMS_ALARM_BYTES);
        return 1;

    default:
        return 0;
    }
}

uint8_t vbms_on_frame(vbms_t *v, const system_model_t *m, uint32_t id,
                      const uint8_t *data, uint8_t len,
                      can_frame_out_t out[VBMS_MAX_FRAMES])
{
    (void)data;   /* Daly requests carry eight reserved bytes */
    (void)len;

    if (v->addr == 0 || ((id >> 24) & 0x1Fu) != DALY_PRIO ||
        ((id >> 8) & 0xFFu) != v->addr) {
        return 0;
    }
    const uint8_t cmd       = (uint8_t)(id >> 16);
    const uint8_t requester = (uint8_t)id;

    bms_pack_t agg;
    vbms_aggregate(m, &agg);
    if (!agg.online) {
        return 0;
    }

    uint8_t frames[VBMS_MAX_FRAMES][8];
    const uint8_t n = encode(&agg, cmd, v->life, frames);
    if (n == 0) {
        return 0;
    }
    if (cmd == DALY_CMD_MOS) {
        v->life++;
    }
    v->requests++;

    for (uint8_t f = 0; f < n; f++) {
        out[f].id  = daly_response_id(cmd, v->addr, requester);
        out[f].len = 8;
        memcpy(out[f].data, frames[f], 8);
    }
    return n;
}

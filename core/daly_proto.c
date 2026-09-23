#include "daly_proto.h"

#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

uint32_t daly_request_id(daly_cmd_t cmd, uint8_t bms_addr)
{
    return ((uint32_t)DALY_PRIO << 24) | ((uint32_t)(cmd & 0xFFu) << 16) |
           ((uint32_t)bms_addr << 8)   | DALY_HOST_ADDR;
}

bool daly_decode_id(uint32_t id, uint8_t *cmd, uint8_t *src_addr)
{
    if (((id >> 24) & 0xFFu) != DALY_PRIO) {
        return false;
    }
    if (((id >> 8) & 0xFFu) != DALY_HOST_ADDR) {
        return false; /* not addressed to us */
    }
    *cmd      = (uint8_t)((id >> 16) & 0xFFu);
    *src_addr = (uint8_t)(id & 0xFFu);
    return true;
}

/* --- multi-frame reassembly ------------------------------------------------
 *
 * Frame indices are 1-based. We track arrivals in a bitmap and only publish
 * once every expected index is present, so an out-of-order or dropped frame
 * costs us one poll round rather than showing a partly-filled cell array.
 *
 * Each command owns its bitmap - see the comment on daly_rx_scratch_t.
 */

static void seen_reset(uint8_t *seen)
{
    memset(seen, 0, DALY_SEEN_BYTES);
}

static void seen_mark(uint8_t *seen, uint8_t idx)
{
    seen[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

static bool seen_have_all(const uint8_t *seen, uint8_t frames)
{
    for (uint8_t i = 1; i <= frames; i++) {
        if (!(seen[i >> 3] & (1u << (i & 7u)))) {
            return false;
        }
    }
    return true;
}

/* Number of frames needed to carry `items` at `per_frame` items each. */
static uint8_t frames_needed(uint8_t items, uint8_t per_frame)
{
    return (uint8_t)((items + per_frame - 1) / per_frame);
}

static bool apply_cell_volts(bms_pack_t *pack, const uint8_t *d)
{
    const uint8_t idx = d[0];

    /* Without a cell count from 0x94 we cannot know when the set is complete. */
    if (pack->cell_count == 0 || pack->cell_count > BMS_MAX_CELLS) {
        return false;
    }
    const uint8_t frames = frames_needed(pack->cell_count, DALY_CELLS_PER_FRAME);
    if (idx < 1 || idx > frames) {
        return false;
    }

    for (uint8_t slot = 0; slot < DALY_CELLS_PER_FRAME; slot++) {
        const uint16_t cell = (uint16_t)((idx - 1) * DALY_CELLS_PER_FRAME + slot);
        if (cell >= pack->cell_count) {
            break; /* padding in the final frame */
        }
        pack->rx.cell_mv[cell] = be16(&d[1 + slot * 2]);
    }
    seen_mark(pack->rx.seen_cells, idx);

    if (!seen_have_all(pack->rx.seen_cells, frames)) {
        return false;
    }
    memcpy(pack->cell_mv, pack->rx.cell_mv,
           sizeof(uint16_t) * pack->cell_count);
    pack->cells_valid = true;
    seen_reset(pack->rx.seen_cells);
    return true;
}

static bool apply_cell_temps(bms_pack_t *pack, const uint8_t *d)
{
    const uint8_t idx = d[0];

    if (pack->temp_count == 0 || pack->temp_count > BMS_MAX_TEMPS) {
        return false;
    }
    const uint8_t frames = frames_needed(pack->temp_count, DALY_TEMPS_PER_FRAME);
    if (idx < 1 || idx > frames) {
        return false;
    }

    for (uint8_t slot = 0; slot < DALY_TEMPS_PER_FRAME; slot++) {
        const uint16_t s = (uint16_t)((idx - 1) * DALY_TEMPS_PER_FRAME + slot);
        if (s >= pack->temp_count) {
            break;
        }
        pack->rx.temp_c[s] = daly_decode_temp_c(d[1 + slot]);
    }
    seen_mark(pack->rx.seen_temps, idx);

    if (!seen_have_all(pack->rx.seen_temps, frames)) {
        return false;
    }
    memcpy(pack->temp_c, pack->rx.temp_c, sizeof(int8_t) * pack->temp_count);
    pack->temps_valid = true;
    seen_reset(pack->rx.seen_temps);
    return true;
}

bool daly_apply_frame(bms_pack_t *pack, uint8_t cmd, const uint8_t d[8])
{
    switch (cmd) {
    case DALY_CMD_SOC:
        pack->pack_mv     = (int32_t)be16(&d[0]) * 100; /* 0.1 V -> mV */
        pack->pack_ma     = daly_decode_current_ma(be16(&d[4]));
        pack->soc_pct_x10 = be16(&d[6]);
        pack->soc_valid   = true;
        return true;

    case DALY_CMD_CELL_MINMAX:
        pack->cell_max_mv  = be16(&d[0]);
        pack->cell_max_idx = d[2];
        pack->cell_min_mv  = be16(&d[3]);
        pack->cell_min_idx = d[5];
        return true;

    case DALY_CMD_TEMP_MINMAX:
        pack->temp_max_c   = daly_decode_temp_c(d[0]);
        pack->temp_max_idx = d[1];
        pack->temp_min_c   = daly_decode_temp_c(d[2]);
        pack->temp_min_idx = d[3];
        pack->temp_minmax_valid = true;
        return true;

    case DALY_CMD_MOS:
        pack->charge_state  = d[0];
        pack->chg_mos       = d[1] != 0;
        pack->dsg_mos       = d[2] != 0;
        pack->bms_life      = d[3];
        pack->remaining_mah = be32(&d[4]);
        return true;

    case DALY_CMD_STATUS: {
        const uint8_t cells = d[0];
        const uint8_t temps = d[1];
        if (cells == 0 || cells > BMS_MAX_CELLS ||
            temps == 0 || temps > BMS_MAX_TEMPS) {
            return false; /* implausible; ignore rather than corrupt the model */
        }
        /* A changed geometry invalidates any set we were assembling. */
        if (cells != pack->cell_count || temps != pack->temp_count) {
            seen_reset(pack->rx.seen_cells);
            seen_reset(pack->rx.seen_temps);
            pack->cells_valid = false;
            pack->temps_valid = false;
        }
        pack->cell_count      = cells;
        pack->temp_count      = temps;
        pack->charger_present = d[2] != 0;
        pack->load_present    = d[3] != 0;
        pack->cycles          = be16(&d[5]);
        return true;
    }

    case DALY_CMD_CELL_VOLTS:
        return apply_cell_volts(pack, d);

    case DALY_CMD_CELL_TEMPS:
        return apply_cell_temps(pack, d);

    case DALY_CMD_BALANCE: {
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) {
            bits |= (uint64_t)d[i] << (8 * i);
        }
        pack->balance_bits = bits;
        return true;
    }

    case DALY_CMD_FAULTS: {
        bool any = false;
        for (int i = 0; i < BMS_ALARM_BYTES; i++) {
            pack->alarms[i] = d[i];
            if (d[i] != 0) {
                any = true;
            }
        }
        pack->alarm_active = any;
        return true;
    }

    default:
        return false;
    }
}

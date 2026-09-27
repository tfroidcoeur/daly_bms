#include "daly_proto.h"

#include <stdio.h>
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
 * We track arrivals in a bitmap and only publish once every expected position
 * is present, so an out-of-order or dropped frame costs us one poll round
 * rather than showing a partly-filled cell array.
 *
 * Each command owns its bitmap - see the comment on daly_rx_scratch_t.
 */

static void seen_reset(uint8_t *seen)
{
    memset(seen, 0, DALY_SEEN_BYTES);
}

static void seen_mark(uint8_t *seen, uint8_t pos)
{
    seen[pos >> 3] |= (uint8_t)(1u << (pos & 7u));
}

static bool seen_have_all(const uint8_t *seen, uint8_t frames)
{
    for (uint8_t i = 0; i < frames; i++) {
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

/*
 * Resolve byte 0 of a multi-frame response into a position within the burst,
 * counted from 0.
 *
 * The two sources disagree about where the wire numbering starts: Daly's own
 * "CAN Communications Protocol V1.0" says byte 0 counts from 0 and that 0xFF
 * means the frame is invalid, while the field layout this project was built
 * from - and, by report, most firmware in service - counts from 1. Guessing
 * wrong is not a near miss. Assume 1 against a zero-based pack and the first
 * frame of every burst is thrown away, so the set never completes and no cell
 * data is ever published; assume 0 against a one-based pack and the same
 * happens at the other end.
 *
 * So we learn it instead, from the only two observations that can come from one
 * scheme and not the other:
 *
 *   byte 0 == 0        a one-based burst never sends this
 *   byte 0 == frames   a zero-based burst stops one short of its own count
 *
 * Anything between is consistent with both and is discarded while we are still
 * undecided, because a frame we cannot place is a frame we must not store. A
 * zero-based pack therefore settles it on the first frame of its first burst
 * and loses nothing; a one-based pack settles it on the last, spending one
 * polling round, once, before its cells appear.
 *
 * `*base` is a daly_frame_base_t, latched in place. Returns false if the frame
 * cannot be placed, in which case `*pos` is untouched.
 */
static bool frame_position(uint8_t *base, uint8_t idx, uint8_t frames,
                           uint8_t *pos)
{
    if (*base == DALY_FRAME_BASE_UNKNOWN) {
        if (idx == 0) {
            *base = DALY_FRAME_BASE_ZERO;
        } else if (idx == frames) {
            *base = DALY_FRAME_BASE_ONE;
        } else {
            return false;
        }
    }

    const uint8_t first = (uint8_t)(*base - DALY_FRAME_BASE_ZERO);
    if (idx < first || idx - first >= frames) {
        return false;   /* out of range, which is where 0xFF lands too */
    }
    *pos = (uint8_t)(idx - first);
    return true;
}

static bool apply_cell_volts(bms_pack_t *pack, const uint8_t *d)
{
    uint8_t pos;

    /* Without a cell count from 0x94 we cannot know when the set is complete -
     * nor, for that matter, which byte 0 the last frame of a burst carries. */
    if (pack->cell_count == 0 || pack->cell_count > BMS_MAX_CELLS) {
        return false;
    }
    const uint8_t frames = frames_needed(pack->cell_count, DALY_CELLS_PER_FRAME);
    if (!frame_position(&pack->rx.base_cells, d[0], frames, &pos)) {
        return false;
    }

    for (uint8_t slot = 0; slot < DALY_CELLS_PER_FRAME; slot++) {
        const uint16_t cell = (uint16_t)(pos * DALY_CELLS_PER_FRAME + slot);
        if (cell >= pack->cell_count) {
            break; /* padding in the final frame */
        }
        pack->rx.cell_mv[cell] = be16(&d[1 + slot * 2]);
    }
    seen_mark(pack->rx.seen_cells, pos);

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
    uint8_t pos;

    if (pack->temp_count == 0 || pack->temp_count > BMS_MAX_TEMPS) {
        return false;
    }
    const uint8_t frames = frames_needed(pack->temp_count, DALY_TEMPS_PER_FRAME);
    if (!frame_position(&pack->rx.base_temps, d[0], frames, &pos)) {
        return false;
    }

    for (uint8_t slot = 0; slot < DALY_TEMPS_PER_FRAME; slot++) {
        const uint16_t s = (uint16_t)(pos * DALY_TEMPS_PER_FRAME + slot);
        if (s >= pack->temp_count) {
            break;
        }
        pack->rx.temp_c[s] = daly_decode_temp_c(d[1 + slot]);
    }
    seen_mark(pack->rx.seen_temps, pos);

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
        /*
         * A changed geometry invalidates any set we were assembling - and the
         * frame count with it, which is half of how the frame base is
         * recognised. The base itself is left alone: it is a property of the
         * firmware, not of the burst.
         */
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

        /*
         * Bytes 5-7 are reserved in Daly's own protocol document, and a cycle
         * count in the layout this project was built from. Take it only when
         * there is something there, so a pack that really does reserve those
         * bytes reports no cycle count rather than a convincing zero - and,
         * once a pack has shown it fills them, do not let a later empty frame
         * unsay it.
         */
        const uint16_t cycles = be16(&d[5]);
        if (cycles != 0) {
            pack->cycles       = cycles;
            pack->cycles_valid = true;
        }
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

/* --- 0x98 fault bits ---------------------------------------------------------
 *
 * Daly's own terms, where they are less plain, in the right-hand comments.
 */

/* Bytes 0-3: one name per level-1/level-2 pair, so index = byte * 4 + bit / 2. */
static const char *const k_level_pairs[4][4] = {
    { "cell voltage high",   "cell voltage low",    /* cell volt high/low     */
      "pack voltage high",   "pack voltage low" },  /* sum volt high/low      */
    { "charge temp high",    "charge temp low",
      "discharge temp high", "discharge temp low" },
    { "charge overcurrent",  "discharge overcurrent",
      "SoC high",            "SoC low" },
    { "cell voltage spread", "temperature spread",  /* diff volt / diff temp  */
      NULL,                  NULL },                /* bits 4-7 reserved      */
};

/* Bytes 4-6: one name per bit. */
static const char *const k_faults[3][8] = {
    { "charge MOSFET hot",          "discharge MOSFET hot",
      "charge MOSFET sensor fault", "discharge MOSFET sensor fault",
      "charge MOSFET stuck on",     "discharge MOSFET stuck on",  /* "adhesion" */
      "charge MOSFET open",         "discharge MOSFET open" },
    { "AFE chip fault",             "cell voltage sense lost",    /* "collect dropped" */
      "cell temp sensor fault",     "EEPROM fault",
      "RTC fault",                  "precharge failed",
      "communication fault",        "internal comms fault" },
    { "current sensor fault",       "pack voltage sense fault",
      "short circuit protection",   "low voltage, charge blocked",
      NULL, NULL, NULL, NULL },                                   /* reserved */
};

const char *daly_fault_name(uint8_t byte, uint8_t bit)
{
    if (bit > 7) {
        return NULL;
    }
    if (byte < 4) {
        return k_level_pairs[byte][bit / 2];
    }
    if (byte < BMS_ALARM_BYTES) {
        return k_faults[byte - 4][bit];
    }
    return NULL;
}

bool daly_fault_is_trip(uint8_t byte, uint8_t bit)
{
    /* In the level pairs, the odd bit is level 2. */
    return byte >= 4 || (bit & 1u);
}

uint8_t daly_fault_worst(const uint8_t alarms[BMS_ALARM_BYTES], uint8_t *pos)
{
    uint8_t count = 0;
    int     trip = -1, warn = -1;

    for (uint8_t byte = 0; byte < BMS_ALARM_BYTES; byte++) {
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (!(alarms[byte] & (1u << bit))) {
                continue;
            }
            count++;
            const bool is_trip = daly_fault_is_trip(byte, bit) ||
                                 daly_fault_name(byte, bit) == NULL;
            if (is_trip && trip < 0) {
                trip = byte * 8 + bit;
            } else if (!is_trip && warn < 0) {
                warn = byte * 8 + bit;
            }
        }
    }
    if (count) {
        *pos = (uint8_t)(trip >= 0 ? trip : warn);
    }
    return count;
}

void daly_fault_describe(uint8_t pos, char *buf, size_t n)
{
    const uint8_t byte = pos / 8, bit = pos % 8;
    const char *name = byte < BMS_ALARM_BYTES ? daly_fault_name(byte, bit) : NULL;

    if (name == NULL) {
        snprintf(buf, n, "fault byte %u bit %u", byte, bit);
    } else if (byte < 4 && daly_fault_is_trip(byte, bit)) {
        snprintf(buf, n, "%s, tripped", name);   /* level 2 of a pair */
    } else {
        snprintf(buf, n, "%s", name);
    }
}

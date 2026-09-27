/*
 * Host unit tests for core/. No framework - just a counter and a macro, so this
 * builds anywhere with a C compiler and needs nothing installed.
 */
#include <stdio.h>
#include <string.h>

#include "bms_model.h"
#include "daly_proto.h"
#include "poller.h"
#include "warnings.h"

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        g_checks++;                                                            \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                        \
            g_fail++;                                                          \
            printf("  FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__,       \
                   __LINE__, #a, #b, _a, _b);                                  \
        }                                                                      \
    } while (0)

static void test_identifiers(void)
{
    /* The worked example from docs/hardware/daly-can-protocol.md. */
    CHECK_EQ(daly_request_id(DALY_CMD_SOC, 0x01), 0x18900140u);
    CHECK_EQ(daly_request_id(DALY_CMD_SOC, 0x02), 0x18900240u);
    CHECK_EQ(daly_request_id(DALY_CMD_SOC, 0x03), 0x18900340u);
    CHECK_EQ(daly_request_id(DALY_CMD_CELL_VOLTS, 0x02), 0x18950240u);

    uint8_t cmd = 0, src = 0;
    CHECK(daly_decode_id(0x18904001u, &cmd, &src));
    CHECK_EQ(cmd, 0x90);
    CHECK_EQ(src, 0x01);

    CHECK(daly_decode_id(0x18954003u, &cmd, &src));
    CHECK_EQ(cmd, 0x95);
    CHECK_EQ(src, 0x03);

    /* A request we transmitted is not a response addressed to us. */
    CHECK(!daly_decode_id(0x18900140u, &cmd, &src));
    /* Wrong priority. */
    CHECK(!daly_decode_id(0x19904001u, &cmd, &src));
}

static void test_soc_scaling(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);

    /*
     * 52.8 V, 15.0 A discharge, 87.5 % SoC: voltage 528 (0.1 V), soc 875
     * (0.1 %), and 150 (0.1 A) of discharge on the far side of the 30000 bias.
     *
     * Which side that is depends on DALY_CURRENT_SIGN, because the Daly
     * drivers in circulation disagree about it. The frame is built for
     * whichever sign this binary was compiled with, and the build runs these
     * tests under both, so neither setting can rot.
     */
    const uint16_t raw = (uint16_t)(30000 - 150 * DALY_CURRENT_SIGN);
    const uint8_t d[8] = { 0x02, 0x10, 0, 0, (uint8_t)(raw >> 8),
                           (uint8_t)(raw & 0xFF), 0x03, 0x6B };
    CHECK(daly_apply_frame(&p, DALY_CMD_SOC, d));
    CHECK_EQ(p.pack_mv, 52800);
    CHECK_EQ(p.pack_ma, -15000);       /* negative: discharging, either way */
    CHECK_EQ(p.soc_pct_x10, 875);

    /* The 30000 bias: exactly 30000 is zero current whichever way it points. */
    CHECK_EQ(daly_decode_current_ma(30000), 0);
    CHECK_EQ(daly_decode_current_ma(30100),   10000 * DALY_CURRENT_SIGN);
    CHECK_EQ(daly_decode_current_ma(29000), -100000 * DALY_CURRENT_SIGN);

    CHECK_EQ(bms_pack_watts(&p), -792); /* 52.8 V * -15 A */
}

static void test_temperature_offset(void)
{
    /* The 40 offset, including the negative range it exists to allow. */
    CHECK_EQ(daly_decode_temp_c(40), 0);
    CHECK_EQ(daly_decode_temp_c(65), 25);
    CHECK_EQ(daly_decode_temp_c(20), -20);

    bms_pack_t p;
    memset(&p, 0, sizeof p);
    const uint8_t d[8] = { 68, 2, 62, 1, 0, 0, 0, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_TEMP_MINMAX, d));
    CHECK_EQ(p.temp_max_c, 28);
    CHECK_EQ(p.temp_max_idx, 2);
    CHECK_EQ(p.temp_min_c, 22);
    CHECK_EQ(p.temp_min_idx, 1);
}

static void test_cell_minmax(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);
    /* max 3345 mV @ cell 4, min 3298 mV @ cell 11 */
    const uint8_t d[8] = { 0x0D, 0x11, 4, 0x0C, 0xE2, 11, 0, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_CELL_MINMAX, d));
    CHECK_EQ(p.cell_max_mv, 3345);
    CHECK_EQ(p.cell_max_idx, 4);
    CHECK_EQ(p.cell_min_mv, 3298);
    CHECK_EQ(p.cell_min_idx, 11);
    CHECK_EQ(bms_pack_cell_delta_mv(&p), 47);
}

/*
 * Byte 0 of a 0x95 or 0x96 response is the frame's place in the burst, but the
 * two sources disagree on where the count starts: Daly's own protocol document
 * V1.0 says 0, and the firmware this project has met says 1. The decoder
 * supports both, so every multi-frame test names which one it is speaking.
 */
#define BASE0 0
#define BASE1 1

/*
 * Build the 0x95 frame at zero-based position `pos` of the burst, carrying
 * cells pos*3 .. +2 at 3300 + cell mV. `base` is what the firmware would put in
 * byte 0 for that position.
 */
static void make_cell_frame(uint8_t pos, uint8_t base, uint8_t cells,
                            uint8_t out[8])
{
    memset(out, 0, 8);
    out[0] = (uint8_t)(base + pos);
    for (uint8_t slot = 0; slot < 3; slot++) {
        const uint16_t cell = (uint16_t)(pos * 3 + slot);
        if (cell >= cells) {
            break;
        }
        const uint16_t mv = (uint16_t)(3300 + cell);
        out[1 + slot * 2] = (uint8_t)(mv >> 8);
        out[2 + slot * 2] = (uint8_t)(mv & 0xFF);
    }
}

static void make_temp_frame(uint8_t pos, uint8_t base, uint8_t temps,
                            uint8_t out[8])
{
    memset(out, 0, 8);
    out[0] = (uint8_t)(base + pos);
    for (uint8_t slot = 0; slot < 7; slot++) {
        const uint16_t s = (uint16_t)(pos * 7 + slot);
        if (s >= temps) {
            break;
        }
        out[1 + slot] = (uint8_t)(20 + s + 40);   /* 20+s degrees, +40 bias */
    }
}

/* Send a whole burst in order. True if it published a set. */
static bool send_cell_burst(bms_pack_t *p, uint8_t base, uint8_t cells)
{
    const uint8_t frames = (uint8_t)((cells + 2) / 3);
    bool published = false;
    for (uint8_t pos = 0; pos < frames; pos++) {
        uint8_t f[8];
        make_cell_frame(pos, base, cells, f);
        if (daly_apply_frame(p, DALY_CMD_CELL_VOLTS, f)) {
            published = true;
        }
    }
    return published;
}

static bool send_temp_burst(bms_pack_t *p, uint8_t base, uint8_t temps)
{
    const uint8_t frames = (uint8_t)((temps + 6) / 7);
    bool published = false;
    for (uint8_t pos = 0; pos < frames; pos++) {
        uint8_t f[8];
        make_temp_frame(pos, base, temps, f);
        if (daly_apply_frame(p, DALY_CMD_CELL_TEMPS, f)) {
            published = true;
        }
    }
    return published;
}

static void set_geometry(bms_pack_t *p, uint8_t cells, uint8_t temps)
{
    const uint8_t d[8] = { cells, temps, 0, 0, 0, 0x01, 0x2C, 0 }; /* 300 cycles */
    CHECK(daly_apply_frame(p, DALY_CMD_STATUS, d));
    CHECK_EQ(p->cell_count, cells);
    CHECK_EQ(p->temp_count, temps);
    CHECK(p->cycles_valid);
    CHECK_EQ(p->cycles, 300);
}

static void test_cell_volts_multiframe(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);

    /* Without a cell count, 0x95 must be refused rather than guessed at. */
    uint8_t f[8];
    make_cell_frame(0, BASE0, 16, f);
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
    CHECK(!p.cells_valid);

    set_geometry(&p, 16, 4);   /* 16 cells -> ceil(16/3) = 6 frames */

    /* Frames 1..5 of the burst must not publish: the set is incomplete. */
    for (uint8_t pos = 0; pos < 5; pos++) {
        make_cell_frame(pos, BASE0, 16, f);
        CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
        CHECK(!p.cells_valid);
    }
    /* The sixth completes it. */
    make_cell_frame(5, BASE0, 16, f);
    CHECK(daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
    CHECK(p.cells_valid);
    for (uint8_t c = 0; c < 16; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }
}

/*
 * Nothing in a frame says whether byte 0 counts from 0 or from 1, so the
 * decoder learns it from the only two observations that can come from just one
 * of the schemes - a byte 0 of 0, which a one-based burst never sends, and a
 * byte 0 equal to the frame count, which a zero-based burst never reaches - and
 * places nothing anywhere until it knows.
 *
 * Getting this wrong is not a subtle failure: assume the wrong base and the
 * decisive frame never arrives, so no cell data is ever published at all.
 */
static void test_frame_numbering_base(void)
{
    bms_pack_t p;
    uint8_t f[8];

    /* Zero-based: byte 0 = 0 settles it on the first frame of the first burst,
     * so that burst already publishes. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);
    CHECK(send_cell_burst(&p, BASE0, 24));
    CHECK(p.cells_valid);
    for (uint8_t c = 0; c < 24; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }

    /* One-based: every frame but the last is consistent with either scheme, so
     * the first burst is spent learning and the second is the one that shows.
     * The cost is one polling round, once per pack. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);
    CHECK(!send_cell_burst(&p, BASE1, 24));
    CHECK(!p.cells_valid);
    CHECK(send_cell_burst(&p, BASE1, 24));
    CHECK(p.cells_valid);
    for (uint8_t c = 0; c < 24; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }

    /* Once it is known, the other scheme's indices are simply out of range. */
    make_cell_frame(0, BASE0, 24, f);        /* byte 0 = 0, never sent here */
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));

    /* 0xFF is "frame invalid" in Daly's document, and out of range either way. */
    make_cell_frame(0, BASE1, 24, f);
    f[0] = 0xFF;
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));

    /* Temperatures are learnt separately: 0x96 carries no evidence about 0x95,
     * and a bitmap shared between them was a bug once already. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);      /* 16 sensors -> 3 frames */
    CHECK(send_temp_burst(&p, BASE0, 16));
    CHECK(p.temps_valid);
    for (uint8_t t = 0; t < 16; t++) {
        CHECK_EQ(p.temp_c[t], 20 + t);
    }

    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);
    CHECK(!send_temp_burst(&p, BASE1, 16));
    CHECK(send_temp_burst(&p, BASE1, 16));
    CHECK(p.temps_valid);
    for (uint8_t t = 0; t < 16; t++) {
        CHECK_EQ(p.temp_c[t], 20 + t);
    }

    /* A set that fits in one frame costs nothing either way round: with one
     * frame expected, both 0 and 1 are decisive the moment they arrive. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 4);
    CHECK(send_temp_burst(&p, BASE1, 4));
    CHECK(p.temps_valid);

    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 4);
    CHECK(send_temp_burst(&p, BASE0, 4));
    CHECK(p.temps_valid);
}

static void test_cell_volts_out_of_order_and_dropped(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);
    set_geometry(&p, 8, 2);   /* 8 cells -> 3 frames */

    /* Frame 0 arrives first, settling the base, and the other two then come the
     * wrong way round. */
    uint8_t f[8];
    const uint8_t order[3] = { 0, 2, 1 };
    for (int i = 0; i < 3; i++) {
        make_cell_frame(order[i], BASE0, 8, f);
        const bool done = daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f);
        CHECK_EQ(done, i == 2);   /* only the last one completes the set */
    }
    CHECK(p.cells_valid);
    for (uint8_t c = 0; c < 8; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }

    /* A dropped frame leaves the previous good data standing, not a half set. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 8, 2);
    make_cell_frame(0, BASE0, 8, f);
    daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f);
    make_cell_frame(2, BASE0, 8, f);   /* the middle frame lost */
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
    CHECK(!p.cells_valid);

    /* Out-of-range indices are rejected outright. */
    make_cell_frame(0, BASE0, 8, f);
    f[0] = 3;    /* one past the last frame of a zero-based burst of three */
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
    f[0] = 99;
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
}

static void test_temps_multiframe(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);

    uint8_t f[8];
    make_temp_frame(0, BASE0, 16, f);
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f));   /* no geometry yet */
    CHECK(!p.temps_valid);

    set_geometry(&p, 24, 16);   /* 16 sensors -> ceil(16/7) = 3 frames */

    for (uint8_t pos = 0; pos < 2; pos++) {
        make_temp_frame(pos, BASE0, 16, f);
        CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f));
        CHECK(!p.temps_valid);
    }
    make_temp_frame(2, BASE0, 16, f);
    CHECK(daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f));
    CHECK(p.temps_valid);
    for (uint8_t t = 0; t < 16; t++) {
        CHECK_EQ(p.temp_c[t], 20 + t);
    }
}

/*
 * 0x95 and 0x96 are assembled at the same time and must not see each other's
 * progress. They used to share one arrival bitmap, so an abandoned cell burst
 * left marks that let the first temperature frame publish a set that was two
 * thirds missing - as 0 C, which reads as a real measurement.
 */
static void test_multiframe_sets_are_independent(void)
{
    bms_pack_t p;
    uint8_t f[8];

    /* A partial cell set must not complete a temperature set. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);   /* cells: 8 frames, temps: 3 frames */

    for (uint8_t pos = 0; pos < 3; pos++) {   /* 3 of 8, then the burst is lost */
        make_cell_frame(pos, BASE0, 24, f);
        daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f);
    }
    CHECK(!p.cells_valid);

    make_temp_frame(0, BASE0, 16, f);
    CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f));
    CHECK(!p.temps_valid);
    for (uint8_t t = 0; t < 16; t++) {
        CHECK_EQ(p.temp_c[t], 0);   /* nothing published, not even partly */
    }

    /* ...and the cell set it interrupted is still standing. */
    for (uint8_t pos = 3; pos < 8; pos++) {
        make_cell_frame(pos, BASE0, 24, f);
        daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f);
    }
    CHECK(p.cells_valid);
    for (uint8_t c = 0; c < 24; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }

    /* The mirror case: a partial temperature set must not complete a cell set. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);

    make_temp_frame(0, BASE0, 16, f);
    daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f);
    CHECK(!p.temps_valid);

    for (uint8_t pos = 0; pos < 7; pos++) {   /* 7 of the 8 cell frames */
        make_cell_frame(pos, BASE0, 24, f);
        CHECK(!daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
        CHECK(!p.cells_valid);
    }
    make_cell_frame(7, BASE0, 24, f);
    CHECK(daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f));
    CHECK(p.cells_valid);

    /* Fully interleaved, both sets complete and neither is corrupted. */
    memset(&p, 0, sizeof p);
    set_geometry(&p, 24, 16);
    for (uint8_t pos = 0; pos < 8; pos++) {
        make_cell_frame(pos, BASE0, 24, f);
        daly_apply_frame(&p, DALY_CMD_CELL_VOLTS, f);
        if (pos < 3) {
            make_temp_frame(pos, BASE0, 16, f);
            daly_apply_frame(&p, DALY_CMD_CELL_TEMPS, f);
        }
    }
    CHECK(p.cells_valid);
    CHECK(p.temps_valid);
    for (uint8_t c = 0; c < 24; c++) {
        CHECK_EQ(p.cell_mv[c], 3300 + c);
    }
    for (uint8_t t = 0; t < 16; t++) {
        CHECK_EQ(p.temp_c[t], 20 + t);
    }
}

/*
 * Daly's protocol document V1.0 lists bytes 5-7 of 0x94 as reserved, but the
 * field layout this project was built from reads a cycle count from bytes 5-6.
 * Rather than pick a side we read it and record whether there was anything
 * there, so a pack that leaves those bytes empty shows no cycle count instead
 * of a zero that reads as a measurement.
 */
static void test_status_cycles_are_optional(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);

    const uint8_t reserved[8] = { 24, 16, 0, 0, 0, 0, 0, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_STATUS, reserved));
    CHECK(!p.cycles_valid);
    CHECK_EQ(p.cycles, 0);

    const uint8_t counted[8] = { 24, 16, 0, 0, 0, 0x01, 0x2C, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_STATUS, counted));
    CHECK(p.cycles_valid);
    CHECK_EQ(p.cycles, 300);

    /* Having once shown that it reports cycles, a pack does not unsay it with
     * an empty frame - the reading stands rather than blinking out. */
    CHECK(daly_apply_frame(&p, DALY_CMD_STATUS, reserved));
    CHECK(p.cycles_valid);
    CHECK_EQ(p.cycles, 300);
}


static void test_status_rejects_implausible_geometry(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);
    const uint8_t bad[8] = { 200, 4, 0, 0, 0, 0, 0, 0 };  /* 200 cells */
    CHECK(!daly_apply_frame(&p, DALY_CMD_STATUS, bad));
    CHECK_EQ(p.cell_count, 0);

    const uint8_t zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!daly_apply_frame(&p, DALY_CMD_STATUS, zero));
    CHECK_EQ(p.cell_count, 0);
}

static void test_mos_and_faults(void)
{
    bms_pack_t p;
    memset(&p, 0, sizeof p);

    const uint8_t mos[8] = { 1, 1, 1, 0x2A, 0x00, 0x01, 0x86, 0xA0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_MOS, mos));
    CHECK_EQ(p.charge_state, 1);
    CHECK(p.chg_mos);
    CHECK(p.dsg_mos);
    CHECK_EQ(p.bms_life, 0x2A);
    CHECK_EQ(p.remaining_mah, 100000u);

    const uint8_t clear[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_FAULTS, clear));
    CHECK(!p.alarm_active);

    const uint8_t set[8] = { 0, 0x02, 0, 0, 0, 0, 0, 0 };
    CHECK(daly_apply_frame(&p, DALY_CMD_FAULTS, set));
    CHECK(p.alarm_active);
    CHECK_EQ(p.alarms[1], 0x02);

    const uint8_t bal[8] = { 0x05, 0, 0, 0, 0, 0, 0, 0 };  /* cells 1 and 3 */
    CHECK(daly_apply_frame(&p, DALY_CMD_BALANCE, bal));
    CHECK_EQ(p.balance_bits, 0x05u);

    CHECK(!daly_apply_frame(&p, 0x7F, clear));   /* unknown command */
}

static void test_model_addressing_and_summary(void)
{
    system_model_t m;
    bms_model_init(&m);

    CHECK_EQ(m.pack[0].addr, 1);
    CHECK_EQ(m.pack[2].addr, 3);
    CHECK(bms_model_by_addr(&m, 2) == &m.pack[1]);
    CHECK(bms_model_by_addr(&m, 0x40) == NULL);
    CHECK(bms_model_by_addr(&m, 9) == NULL);

    /* Nothing online yet. */
    bms_summary_t s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 0);
    CHECK_EQ(s.bank_mv, 0);

    for (int i = 0; i < 3; i++) {
        m.pack[i].online       = true;
        m.pack[i].pack_mv      = 52000 + i * 100;
        m.pack[i].pack_ma      = -10000;
        m.pack[i].soc_pct_x10  = (uint16_t)(800 + i * 10);
        m.pack[i].cell_min_mv  = 3300;
        m.pack[i].cell_max_mv  = (uint16_t)(3320 + i * 10);
    }
    s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 3);
    CHECK_EQ(s.total_ma, -30000);            /* currents add in parallel */
    CHECK_EQ(s.bank_mv, 52100);              /* voltages average */
    CHECK_EQ(s.mean_soc_pct_x10, 810);
    CHECK_EQ(s.worst_cell_delta_mv, 40);     /* pack 3: 3340 - 3300 */
    CHECK(!s.any_alarm);

    /* Bank power: mean voltage x summed current, not a sum of products. */
    CHECK_EQ(bms_summary_watts(&s), -1563);  /* 52.1 V * -30 A */

    /* Charge state comes from the packs, and charging outranks discharging. */
    CHECK_EQ(s.charge_state, 0);             /* nothing reported yet */
    for (int i = 0; i < 3; i++) {
        m.pack[i].charge_state = 2;          /* all discharging */
    }
    s = bms_model_summary(&m);
    CHECK_EQ(s.charge_state, 2);
    CHECK(!s.charger_present);

    m.pack[1].charge_state    = 1;           /* one pack taking charge */
    m.pack[1].charger_present = true;
    s = bms_model_summary(&m);
    CHECK_EQ(s.charge_state, 1);
    CHECK(s.charger_present);

    /* An offline pack contributes nothing, including its state. */
    m.pack[1].online = false;
    s = bms_model_summary(&m);
    CHECK_EQ(s.charge_state, 2);
    CHECK(!s.charger_present);

    /* Put the pack back: the assertions below assume all three are online. */
    m.pack[1].online          = true;
    m.pack[1].charge_state    = 2;
    m.pack[1].charger_present = false;

    /* An offline pack drops out of every aggregate. */
    m.pack[2].online = false;
    s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 2);
    CHECK_EQ(s.total_ma, -20000);
    CHECK_EQ(s.worst_cell_delta_mv, 30);
}

static void test_ageing(void)
{
    system_model_t m;
    bms_model_init(&m);
    for (int i = 0; i < 3; i++) {
        m.pack[i].online       = true;
        m.pack[i].last_seen_ms = 1000;
    }

    bms_model_age(&m, 5000, 5000);
    CHECK(m.pack[0].online);          /* exactly at the limit: still online */

    bms_model_age(&m, 6001, 5000);
    CHECK(!m.pack[0].online);
    CHECK(!m.pack[1].online);

    /* Ageing must survive the 32-bit millisecond wrap. */
    bms_model_init(&m);
    m.pack[0].online       = true;
    m.pack[0].last_seen_ms = 0xFFFFF000u;
    bms_model_age(&m, 0x00000100u, 5000);   /* 4352 ms later, across the wrap */
    CHECK(m.pack[0].online);
}

static void test_poller_round_robin(void)
{
    system_model_t m;
    poller_t p;
    can_frame_out_t f;

    bms_model_init(&m);
    poller_init(&p, &m, poller_default_cfg());

    /* First request of all is the status read for pack 1 - it gates 0x95. */
    CHECK(poller_tick(&p, 0, &f));
    CHECK_EQ(f.id, daly_request_id(DALY_CMD_STATUS, 1));
    CHECK_EQ(f.len, 8);

    /* Nothing more until the inter-request gap has elapsed. */
    CHECK(!poller_tick(&p, 10, &f));
    CHECK(!poller_tick(&p, 39, &f));

    /* Walk a whole round and confirm every pack gets asked. */
    int per_pack[4] = { 0 };
    uint32_t t = 40;
    for (int i = 0; i < 200; i++, t += 40) {
        if (poller_tick(&p, t, &f)) {
            const uint8_t addr = (uint8_t)((f.id >> 8) & 0xFF);
            CHECK(addr >= 1 && addr <= 3);
            per_pack[addr]++;
        }
    }
    CHECK(per_pack[1] > 0);
    CHECK(per_pack[2] > 0);
    CHECK(per_pack[3] > 0);
    /* Fair share: no pack starved relative to the others. */
    CHECK(per_pack[1] - per_pack[3] <= 2);
}

static void test_poller_gates_cell_reads_on_geometry(void)
{
    system_model_t m;
    poller_t p;
    can_frame_out_t f;

    bms_model_init(&m);
    poller_init(&p, &m, poller_default_cfg());

    /* With no cell count known, 0x95 must never be requested. */
    uint32_t t = 0;
    for (int i = 0; i < 100; i++, t += 40) {
        if (poller_tick(&p, t, &f)) {
            CHECK(((f.id >> 16) & 0xFF) != DALY_CMD_CELL_VOLTS);
        }
    }

    /* Once pack 2 reports its geometry, it starts being asked for cells. */
    const uint8_t status[8] = { 16, 4, 0, 0, 0, 0, 0, 0 };
    CHECK(poller_on_frame(&p, 0x18944002u, status, 8, t));

    bool asked = false;
    for (int i = 0; i < 200; i++, t += 40) {
        if (poller_tick(&p, t, &f) &&
            ((f.id >> 16) & 0xFF) == DALY_CMD_CELL_VOLTS) {
            CHECK_EQ((f.id >> 8) & 0xFF, 2);   /* only pack 2 knows its geometry */
            asked = true;
        }
    }
    CHECK(asked);
}

static void test_poller_liveness(void)
{
    system_model_t m;
    poller_t p;
    can_frame_out_t f;

    bms_model_init(&m);
    poller_cfg_t cfg = poller_default_cfg();
    cfg.offline_timeout_ms = 1000;
    poller_init(&p, &m, cfg);

    CHECK(!m.pack[0].online);

    const uint8_t soc[8] = { 0x02, 0x10, 0, 0, 0x75, 0x30, 0x03, 0xE8 };
    CHECK(poller_on_frame(&p, 0x18904001u, soc, 8, 500));
    CHECK(m.pack[0].online);
    CHECK_EQ(m.pack[0].last_seen_ms, 500u);
    CHECK_EQ(m.pack[0].soc_pct_x10, 1000);
    CHECK_EQ(m.pack[0].frames_rx, 1u);

    /* Frames from an unknown address are ignored entirely. */
    CHECK(!poller_on_frame(&p, 0x18904009u, soc, 8, 500));
    /* So are short frames. */
    CHECK(!poller_on_frame(&p, 0x18904001u, soc, 4, 500));
    /* And our own outgoing requests, echoed back. */
    CHECK(!poller_on_frame(&p, 0x18900140u, soc, 8, 500));

    /* Silence past the timeout drops the pack offline on the next tick. */
    poller_tick(&p, 1600, &f);
    CHECK(!m.pack[0].online);

    /* And it comes straight back when it speaks again. */
    CHECK(poller_on_frame(&p, 0x18904001u, soc, 8, 1700));
    CHECK(m.pack[0].online);
}


/* ---- warnings ------------------------------------------------------------ */

/* A healthy three-pack bank: everything nominal, nothing to report. */
static void make_healthy(system_model_t *m)
{
    bms_model_init(m);
    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        bms_pack_t *p = &m->pack[i];
        p->online       = true;
        p->pack_mv      = 53000;
        p->pack_ma      = -12000;
        p->charge_state = 2;          /* discharging, as 0x93 would say */
        p->soc_pct_x10  = 800;
        p->cell_count   = 16;
        p->temp_count   = 4;
        p->cell_min_mv  = 3310;
        p->cell_max_mv  = 3330;
        p->temp_min_c   = 20;
        p->temp_max_c   = 26;
        p->cells_valid  = true;
        p->soc_valid          = true;
        p->temp_minmax_valid  = true;
        for (int c = 0; c < 16; c++) {
            p->cell_mv[c] = (uint16_t)(3310 + (c % 3) * 10);
        }
    }
}

static bool has_code(const warning_set_t *s, warn_code_t code, uint8_t pack)
{
    for (uint8_t i = 0; i < s->count; i++) {
        if (s->item[i].code == code && s->item[i].pack == pack) {
            return true;
        }
    }
    return false;
}

static void test_warnings_clean_bank(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    warnings_evaluate(&m, &s);
    CHECK_EQ(s.count, 0);
    CHECK(!warnings_any_alarm(&s));
    CHECK(!s.truncated);
}

static void test_warnings_offline_pack(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    m.pack[1].online = false;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_PACK_OFFLINE, 2));
    CHECK(warnings_any_alarm(&s));
}

static void test_warnings_parallel_voltage_mismatch(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    /* Just under the threshold: packs in parallel always differ a little. */
    m.pack[2].pack_mv = 53000 + WARN_PACK_MV_MISMATCH - 1;
    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_PACK_VOLTAGE_MISMATCH, 0));

    /* Over it: something is between the packs. */
    m.pack[2].pack_mv = 53000 + WARN_PACK_MV_MISMATCH;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_PACK_VOLTAGE_MISMATCH, 0));

    /* A single online pack cannot mismatch against anything. */
    make_healthy(&m);
    m.pack[1].online = false;
    m.pack[2].online = false;
    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_PACK_VOLTAGE_MISMATCH, 0));
}

static void test_warnings_cell_anomalies(void)
{
    system_model_t m;
    warning_set_t s;

    /* A missing sense lead reads near zero - a broken measurement, not a low
     * cell, so it must be reported as such. */
    make_healthy(&m);
    m.pack[0].cell_mv[4] = 0;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_IMPLAUSIBLE, 1));

    /* Implausibly high too. */
    make_healthy(&m);
    m.pack[0].cell_mv[9] = 4500;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_IMPLAUSIBLE, 1));

    /* Cell data we have not received yet must not be scanned. */
    make_healthy(&m);
    m.pack[0].cells_valid = false;
    m.pack[0].cell_mv[4]  = 0;
    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_CELL_IMPLAUSIBLE, 1));

    /* Cell count disagreeing with the other packs. */
    make_healthy(&m);
    m.pack[1].cell_count = 15;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_COUNT_MISMATCH, 2));
}

static void test_warnings_spread_levels(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    m.pack[0].cell_max_mv = (uint16_t)(m.pack[0].cell_min_mv +
                                       WARN_CELL_SPREAD_MV);
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_SPREAD, 1));
    CHECK(!warnings_any_alarm(&s));   /* warn, not alarm */

    m.pack[0].cell_max_mv = (uint16_t)(m.pack[0].cell_min_mv +
                                       WARN_CELL_SPREAD_ALARM_MV);
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_SPREAD, 1));
    CHECK(warnings_any_alarm(&s));
}

static void test_warnings_soc_and_temperature(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    m.pack[0].soc_pct_x10 = WARN_SOC_LOW_PCT_X10;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_SOC_LOW, 1));
    CHECK(!warnings_any_alarm(&s));

    m.pack[0].soc_pct_x10 = WARN_SOC_CRIT_PCT_X10;
    warnings_evaluate(&m, &s);
    CHECK(warnings_any_alarm(&s));

    make_healthy(&m);
    m.pack[1].temp_max_c = WARN_TEMP_HIGH_ALARM_C;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_TEMP_HIGH, 2));
    CHECK(warnings_any_alarm(&s));

    make_healthy(&m);
    m.pack[1].temp_min_c = WARN_TEMP_LOW_C;
    m.pack[1].pack_ma    = -5000;      /* discharging: merely cold */
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_TEMP_LOW, 2));
    CHECK(!warnings_any_alarm(&s));
}

static void test_warnings_charging_below_zero(void)
{
    system_model_t m;
    warning_set_t s;

    /* Below freezing while discharging is only a cold warning. */
    make_healthy(&m);
    m.pack[2].temp_min_c   = -3;
    m.pack[2].charge_state = 2;
    m.pack[2].pack_ma      = -8000;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_TEMP_LOW, 3));
    CHECK(!has_code(&s, WARN_CHARGING_BELOW_ZERO, 3));

    /* The same temperature while charging plates lithium: alarm. */
    m.pack[2].charge_state = 1;
    m.pack[2].pack_ma      = +8000;
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CHARGING_BELOW_ZERO, 3));
    CHECK(warnings_any_alarm(&s));

    /*
     * The drivers disagree about which way Daly's current points, so "charging"
     * must come from the BMS's own state in 0x93 and never from the sign. On a
     * pack whose sign runs the other way, the old rule raised this alarm while
     * discharging in the cold - and stayed silent while actually charging
     * below freezing, the one case it exists for.
     */
    m.pack[2].charge_state = 1;
    m.pack[2].pack_ma      = -8000;          /* charging, sign inverted */
    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CHARGING_BELOW_ZERO, 3));

    m.pack[2].charge_state = 2;
    m.pack[2].pack_ma      = +8000;          /* discharging, sign inverted */
    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_CHARGING_BELOW_ZERO, 3));
    CHECK(has_code(&s, WARN_TEMP_LOW, 3));

    /* Before 0x93 has answered the state is unknown, and unknown is not
     * charging - the BMS's own charge-temperature protection covers the gap. */
    m.pack[2].charge_state = 0;
    m.pack[2].pack_ma      = +8000;
    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_CHARGING_BELOW_ZERO, 3));
}

static void test_warnings_ordering_and_capacity(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    m.pack[2].soc_pct_x10 = WARN_SOC_LOW_PCT_X10;   /* warn, pack 3 */
    m.pack[0].online      = false;                  /* alarm, pack 1 */
    warnings_evaluate(&m, &s);

    CHECK(s.count >= 2);
    /* Alarms sort ahead of warnings, so the worst line is always drawn. */
    CHECK_EQ(s.item[0].level, WARN_LEVEL_ALARM);
    CHECK_EQ(s.item[s.count - 1].level, WARN_LEVEL_WARN);

    /* The set never overflows, and says so when it clips. */
    bms_model_init(&m);
    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        bms_pack_t *p = &m.pack[i];
        p->online      = true;
        p->cell_count  = 16;
        p->cells_valid = true;
        p->pack_mv     = 40000 + i * 4000;   /* mismatch */
        p->pack_ma     = 5000;
        p->charge_state = 1;                 /* charging */
        p->soc_pct_x10 = 50;                 /* critical */
        p->temp_min_c  = -20;                /* charging below zero */
        p->temp_max_c  = 70;                 /* too hot */
        p->cell_min_mv = 3000;
        p->cell_max_mv = 3400;               /* spread alarm */
        p->soc_valid         = true;
        p->temp_minmax_valid = true;
        p->alarm_active = true;
        p->alarms[0]    = 0x01;
        for (int c = 0; c < 16; c++) {
            p->cell_mv[c] = 100;             /* implausible */
        }
    }
    warnings_evaluate(&m, &s);
    CHECK(s.count <= WARN_MAX);
    CHECK(s.truncated);
}

/*
 * The set is capped at WARN_MAX. What survives the cap must be chosen by
 * severity, not by arrival order - the bank-level rules are evaluated last, so
 * a naive drop-on-overflow loses exactly the line that matters most.
 */
static void test_warnings_truncation_keeps_the_worst(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    /* Four warn-level items per pack: spread, SoC, hot, cold = 12 = WARN_MAX. */
    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        bms_pack_t *p = &m.pack[i];
        p->cell_max_mv = (uint16_t)(p->cell_min_mv + 150);
        p->soc_pct_x10 = 150;
        p->temp_max_c  = 50;
        p->temp_min_c  = 1;
    }
    warnings_evaluate(&m, &s);
    CHECK_EQ(s.count, WARN_MAX);
    CHECK(!s.truncated);
    CHECK(!warnings_any_alarm(&s));

    /* Now add a bank-level alarm, pushed after all twelve. It must displace a
     * warn-level line rather than being dropped itself. */
    m.pack[1].pack_mv = m.pack[0].pack_mv + 2000;
    warnings_evaluate(&m, &s);
    CHECK_EQ(s.count, WARN_MAX);
    CHECK(s.truncated);
    CHECK(warnings_any_alarm(&s));
    CHECK(has_code(&s, WARN_PACK_VOLTAGE_MISMATCH, 0));
    CHECK_EQ(s.item[0].code, WARN_PACK_VOLTAGE_MISMATCH);
    CHECK_EQ(s.item[0].level, WARN_LEVEL_ALARM);
}

/*
 * A pack is online as soon as it answers anything. Fields it has not reported
 * yet are still zero, and zero is a legal reading for SoC and temperature - so
 * the rules must not fire until the command that fills them has been seen.
 */
static void test_warnings_ignore_unreported_fields(void)
{
    system_model_t m;
    warning_set_t s;
    bms_model_init(&m);

    bms_pack_t *p = &m.pack[0];
    p->online = true;
    set_geometry(p, 16, 4);          /* 0x94 only: no SoC, no temperatures */

    warnings_evaluate(&m, &s);
    CHECK(!has_code(&s, WARN_SOC_LOW, 1));
    CHECK(!has_code(&s, WARN_TEMP_LOW, 1));
    CHECK(!has_code(&s, WARN_TEMP_HIGH, 1));
    CHECK(!has_code(&s, WARN_CHARGING_BELOW_ZERO, 1));

    /* Once the readings actually arrive, the same rules must fire. */
    const uint8_t soc[8] = { 0x02, 0x0C, 0, 0, 0x75, 0x30, 0x00, 0x32 };
    CHECK(daly_apply_frame(p, DALY_CMD_SOC, soc));      /* SoC 5.0 % */
    const uint8_t temp[8] = { 40 + 60, 1, 40 - 10, 2, 0, 0, 0, 0 };
    CHECK(daly_apply_frame(p, DALY_CMD_TEMP_MINMAX, temp));  /* 60 C / -10 C */

    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_SOC_LOW, 1));
    CHECK(has_code(&s, WARN_TEMP_HIGH, 1));
}

/* The cell-count reference must come from a pack that has reported one. */
static void test_warnings_cell_count_reference(void)
{
    system_model_t m;
    warning_set_t s;

    make_healthy(&m);
    m.pack[0].cell_count  = 0;    /* online, but 0x94 has not landed yet */
    m.pack[0].cells_valid = false;
    m.pack[1].cell_count  = 16;
    m.pack[2].cell_count  = 8;    /* disagrees with pack 2 */

    warnings_evaluate(&m, &s);
    CHECK(has_code(&s, WARN_CELL_COUNT_MISMATCH, 3));
}

/*
 * An outage must not leave half a burst behind for the next one to complete:
 * cells 1-9 from before the dropout spliced onto 10-24 from after it would
 * publish as one coherent set.
 */
static void test_multiframe_scratch_cleared_on_offline(void)
{
    system_model_t m;
    bms_model_init(&m);
    bms_pack_t *p = &m.pack[0];

    set_geometry(p, 24, 16);       /* 8 cell frames */
    p->online       = true;
    p->last_seen_ms = 1000;

    uint8_t f[8];
    for (uint8_t pos = 0; pos < 3; pos++) {
        make_cell_frame(pos, BASE0, 24, f);
        daly_apply_frame(p, DALY_CMD_CELL_VOLTS, f);
    }
    CHECK(!p->cells_valid);

    bms_model_age(&m, 1000 + 6000, 5000);
    CHECK(!p->online);

    /* It comes back and resumes mid-burst with only the missing frames. The
     * learnt frame base survives the outage - it is a property of the firmware,
     * not of the burst - so the pack is not made to teach us it twice. */
    p->online = true;
    for (uint8_t pos = 3; pos < 8; pos++) {
        make_cell_frame(pos, BASE0, 24, f);
        CHECK(!daly_apply_frame(p, DALY_CMD_CELL_VOLTS, f));
    }
    CHECK(!p->cells_valid);
}

/* slow_divider is caller-supplied; 0 is a natural way to write "never skip". */
static void test_poller_tolerates_zero_slow_divider(void)
{
    system_model_t m;
    poller_t pl;
    can_frame_out_t tx;

    bms_model_init(&m);
    poller_cfg_t cfg = poller_default_cfg();
    cfg.slow_divider = 0;
    poller_init(&pl, &m, cfg);

    CHECK(poller_tick(&pl, 0, &tx));   /* must not divide by zero */
}

static void test_warnings_formatting(void)
{
    char buf[64];

    const warning_t off = { WARN_PACK_OFFLINE, WARN_LEVEL_ALARM, 2, 0, 0 };
    warnings_format(&off, buf, sizeof buf);
    CHECK(strstr(buf, "PACK 2") != NULL);
    CHECK(strstr(buf, "not responding") != NULL);

    const warning_t mm = { WARN_PACK_VOLTAGE_MISMATCH, WARN_LEVEL_ALARM,
                           0, 1250, 0 };
    warnings_format(&mm, buf, sizeof buf);
    CHECK(strstr(buf, "BANK") != NULL);
    CHECK(strstr(buf, "1.25 V") != NULL);

    const warning_t cell = { WARN_CELL_IMPLAUSIBLE, WARN_LEVEL_ALARM, 1, 5, 0 };
    warnings_format(&cell, buf, sizeof buf);
    CHECK(strstr(buf, "cell 5") != NULL);

    const warning_t soc = { WARN_SOC_LOW, WARN_LEVEL_WARN, 3, 185, 0 };
    warnings_format(&soc, buf, sizeof buf);
    CHECK(strstr(buf, "18.5 %") != NULL);

    /* Every line must fit the display width with room to spare. */
    for (int code = 0; code <= WARN_CHARGING_BELOW_ZERO; code++) {
        const warning_t w = { (warn_code_t)code, WARN_LEVEL_WARN, 3,
                              -12345, 255 };
        warnings_format(&w, buf, sizeof buf);
        CHECK(strlen(buf) > 0);
        CHECK(strlen(buf) < 52);
    }
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "identifiers",                 test_identifiers },
        { "soc scaling / current bias",  test_soc_scaling },
        { "temperature offset",          test_temperature_offset },
        { "cell min/max",                test_cell_minmax },
        { "0x95 multi-frame",            test_cell_volts_multiframe },
        { "0x95/0x96 frame numbering",   test_frame_numbering_base },
        { "0x95 reordered / dropped",    test_cell_volts_out_of_order_and_dropped },
        { "0x96 multi-frame",            test_temps_multiframe },
        { "0x95/0x96 independence",      test_multiframe_sets_are_independent },
        { "0x94 geometry validation",    test_status_rejects_implausible_geometry },
        { "0x94 cycles are optional",    test_status_cycles_are_optional },
        { "MOS / faults / balance",      test_mos_and_faults },
        { "model addressing + summary",  test_model_addressing_and_summary },
        { "ageing (incl. ms wrap)",      test_ageing },
        { "poller round-robin",          test_poller_round_robin },
        { "poller gates cell reads",     test_poller_gates_cell_reads_on_geometry },
        { "poller liveness",             test_poller_liveness },
        { "warnings: clean bank",        test_warnings_clean_bank },
        { "warnings: offline pack",      test_warnings_offline_pack },
        { "warnings: parallel mismatch", test_warnings_parallel_voltage_mismatch },
        { "warnings: cell anomalies",    test_warnings_cell_anomalies },
        { "warnings: spread levels",     test_warnings_spread_levels },
        { "warnings: SoC / temperature", test_warnings_soc_and_temperature },
        { "warnings: charging sub-zero", test_warnings_charging_below_zero },
        { "warnings: order + capacity",  test_warnings_ordering_and_capacity },
        { "warnings: truncation order",  test_warnings_truncation_keeps_the_worst },
        { "warnings: unreported fields", test_warnings_ignore_unreported_fields },
        { "warnings: cell-count ref",    test_warnings_cell_count_reference },
        { "0x95 scratch cleared offline",test_multiframe_scratch_cleared_on_offline },
        { "poller: slow_divider 0",      test_poller_tolerates_zero_slow_divider },
        { "warnings: formatting",        test_warnings_formatting },
    };

    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_fail;
        tests[i].fn();
        printf("%-32s %s\n", tests[i].name,
               g_fail == before ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

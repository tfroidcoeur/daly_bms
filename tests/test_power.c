/*
 * Host unit tests for core/power.c: the LOCAL / REPORT / IDLE state machine.
 * Uses a short config (1 s local, 0.5 s report) except where the defaults are
 * the point.
 */
#include <stdint.h>

#include "bms_model.h"
#include "check.h"
#include "power.h"

int g_fail, g_checks;

static pwr_cfg_t short_cfg(void)
{
    pwr_cfg_t c = { .local_timeout_ms = 1000, .report_timeout_ms = 500 };
    return c;
}

static void expect_out(pwr_out_t o, bool poll, bool live, bool upload, bool sleep)
{
    CHECK_EQ(o.poll_can, poll);
    CHECK_EQ(o.screen_live, live);
    CHECK_EQ(o.upload_now, upload);
    CHECK_EQ(o.sleep_now, sleep);
}

/* Pointer to one of the five flags pwr_snapshot_complete() looks at. */
static bool *complete_flag(bms_pack_t *k, int f)
{
    switch (f) {
    case 0:  return &k->online;
    case 1:  return &k->soc_valid;
    case 2:  return &k->cells_valid;
    case 3:  return &k->temps_valid;
    default: return &k->temp_minmax_valid;
    }
}

static void set_complete(system_model_t *m)
{
    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        for (int f = 0; f < 5; f++) {
            *complete_flag(&m->pack[i], f) = true;
        }
    }
}

static void test_wake_causes(void)
{
    const remote_wake_t live[] = { REMOTE_WAKE_BOOT, REMOTE_WAKE_TOUCH,
                                   REMOTE_WAKE_CAN };
    for (unsigned i = 0; i < sizeof live / sizeof live[0]; i++) {
        pwr_t p;
        pwr_init(&p, short_cfg(), live[i], 0);
        CHECK_EQ(p.state, PWR_LOCAL);
        expect_out(pwr_tick(&p, 0, false), true, true, false, false);
    }

    pwr_t t;
    pwr_init(&t, short_cfg(), REMOTE_WAKE_TIMER, 0);
    CHECK_EQ(t.state, PWR_IDLE);
    expect_out(pwr_tick(&t, 0, false), false, false, false, true);
}

static void test_local_timeout(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_BOOT, 0);
    expect_out(pwr_tick(&p, 999, false), true, true, false, false);
    expect_out(pwr_tick(&p, 1000, false), false, false, false, true);
    CHECK_EQ(p.state, PWR_IDLE);

    pwr_t t;
    pwr_init(&t, short_cfg(), REMOTE_WAKE_BOOT, 0);
    pwr_on_touch(&t, 800);
    expect_out(pwr_tick(&t, 1799, false), true, true, false, false);
    expect_out(pwr_tick(&t, 1800, false), false, false, false, true);

    pwr_t v;
    pwr_init(&v, short_cfg(), REMOTE_WAKE_BOOT, 0);
    pwr_on_vbms_request(&v, 800);
    expect_out(pwr_tick(&v, 1799, false), true, true, false, false);
    expect_out(pwr_tick(&v, 1800, false), false, false, false, true);
}

static void test_idle_wakes_to_local(void)
{
    pwr_t t;
    pwr_init(&t, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_touch(&t, 5000);
    CHECK_EQ(t.state, PWR_LOCAL);
    expect_out(pwr_tick(&t, 5000, false), true, true, false, false);

    pwr_t v;
    pwr_init(&v, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_vbms_request(&v, 5000);
    CHECK_EQ(v.state, PWR_LOCAL);
    expect_out(pwr_tick(&v, 5000, false), true, true, false, false);
}

static void test_report_from_idle(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_report_cmd(&p, 100);
    CHECK_EQ(p.state, PWR_REPORT);
    expect_out(pwr_tick(&p, 100, false), true, false, false, false);

    expect_out(pwr_tick(&p, 200, true), true, false, true, false);
    expect_out(pwr_tick(&p, 201, true), true, false, false, false);

    pwr_on_upload_done(&p, 210);
    CHECK_EQ(p.state, PWR_IDLE);
    expect_out(pwr_tick(&p, 211, true), false, false, false, true);
}

static void test_report_timeout(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_report_cmd(&p, 100);
    expect_out(pwr_tick(&p, 599, false), true, false, false, false);
    expect_out(pwr_tick(&p, 600, false), true, false, true, false);
    expect_out(pwr_tick(&p, 601, false), true, false, false, false);
}

static void test_touch_during_report(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_report_cmd(&p, 0);
    pwr_on_touch(&p, 50);
    CHECK_EQ(p.state, PWR_LOCAL);
    expect_out(pwr_tick(&p, 60, false), true, true, false, false);

    expect_out(pwr_tick(&p, 70, true), true, true, true, false);
    expect_out(pwr_tick(&p, 71, true), true, true, false, false);

    pwr_on_upload_done(&p, 80);
    CHECK_EQ(p.state, PWR_LOCAL);
    expect_out(pwr_tick(&p, 81, false), true, true, false, false);
    expect_out(pwr_tick(&p, 1049, false), true, true, false, false);
    expect_out(pwr_tick(&p, 1050, false), false, false, false, true);
}

static void test_report_while_local(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_BOOT, 0);
    pwr_on_report_cmd(&p, 900);
    CHECK_EQ(p.state, PWR_LOCAL);

    /* Past the local timeout, but the report is still wanted: stay live. */
    expect_out(pwr_tick(&p, 1399, false), true, true, false, false);
    expect_out(pwr_tick(&p, 1400, false), true, true, true, false);
    expect_out(pwr_tick(&p, 1401, false), true, true, false, false);

    pwr_on_upload_done(&p, 1450);
    CHECK_EQ(p.state, PWR_LOCAL);
    expect_out(pwr_tick(&p, 1451, false), false, false, false, true);
    CHECK_EQ(p.state, PWR_IDLE);
}

static void test_second_report_keeps_since(void)
{
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_report_cmd(&p, 100);
    pwr_on_report_cmd(&p, 300);
    CHECK_EQ(p.report_since_ms, 100);
    expect_out(pwr_tick(&p, 599, false), true, false, false, false);
    expect_out(pwr_tick(&p, 600, false), true, false, true, false);
}

static void test_wraparound(void)
{
    const uint32_t t0 = UINT32_MAX - 100;

    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_BOOT, t0);
    expect_out(pwr_tick(&p, (uint32_t)(t0 + 999), false), true, true, false, false);
    expect_out(pwr_tick(&p, (uint32_t)(t0 + 1000), false), false, false, false, true);

    pwr_t r;
    pwr_init(&r, short_cfg(), REMOTE_WAKE_TIMER, t0);
    const uint32_t asked = UINT32_MAX - 10;
    pwr_on_report_cmd(&r, asked);
    CHECK_EQ(r.report_since_ms, asked);
    expect_out(pwr_tick(&r, (uint32_t)(asked + 499), false), true, false, false, false);
    expect_out(pwr_tick(&r, (uint32_t)(asked + 500), false), true, false, true, false);
}

static void test_snapshot_complete(void)
{
    system_model_t m;
    bms_model_init(&m);
    CHECK(!pwr_snapshot_complete(&m));

    set_complete(&m);
    CHECK(pwr_snapshot_complete(&m));

    /* Any single flag missing on any pack makes the snapshot incomplete. */
    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        for (int f = 0; f < 5; f++) {
            bool *flag = complete_flag(&m.pack[i], f);
            *flag = false;
            CHECK(!pwr_snapshot_complete(&m));
            *flag = true;
        }
    }
    CHECK(pwr_snapshot_complete(&m));
}

static void test_bank_active(void)
{
    /* Current flowing keeps the screen on, like a touch. */
    pwr_t p;
    pwr_init(&p, short_cfg(), REMOTE_WAKE_BOOT, 0);
    pwr_on_bank_active(&p, 900);
    expect_out(pwr_tick(&p, 1899, false), true, true, false, false);
    expect_out(pwr_tick(&p, 1900, false), false, false, false, true);

    /* It never wakes the screen: in IDLE nothing changes. */
    pwr_on_bank_active(&p, 2000);
    CHECK_EQ(p.state, PWR_IDLE);
    expect_out(pwr_tick(&p, 2000, false), false, false, false, true);

    /* Nor does it turn a report into a live screen. */
    pwr_t r;
    pwr_init(&r, short_cfg(), REMOTE_WAKE_TIMER, 0);
    pwr_on_report_cmd(&r, 100);
    pwr_on_bank_active(&r, 150);
    CHECK_EQ(r.state, PWR_REPORT);

    /* Which packs count: online and charging (1) or discharging (2). */
    system_model_t m;
    bms_model_init(&m);
    CHECK(!pwr_bank_active(&m));
    m.pack[1].online = true;
    m.pack[1].charge_state = 0;
    CHECK(!pwr_bank_active(&m));
    m.pack[1].charge_state = 1;
    CHECK(pwr_bank_active(&m));
    m.pack[1].charge_state = 2;
    CHECK(pwr_bank_active(&m));
    m.pack[1].online = false;          /* an offline pack's last word is old */
    CHECK(!pwr_bank_active(&m));
    m.pack[2].online = true;
    m.pack[2].charge_state = 3;        /* not a state Daly sends */
    CHECK(!pwr_bank_active(&m));
}

static void test_defaults(void)
{
    pwr_cfg_t c = pwr_default_cfg();
    CHECK_EQ(c.local_timeout_ms, 300000);
    CHECK_EQ(c.report_timeout_ms, 60000);

    pwr_t p;
    pwr_init(&p, c, REMOTE_WAKE_BOOT, 0);
    expect_out(pwr_tick(&p, 299999, false), true, true, false, false);
    expect_out(pwr_tick(&p, 300000, false), false, false, false, true);
}

int main(void)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "wake causes",                 test_wake_causes },
        { "LOCAL timeout + touch/vbms",  test_local_timeout },
        { "IDLE + touch/vbms -> LOCAL",  test_idle_wakes_to_local },
        { "REPORT from IDLE",            test_report_from_idle },
        { "REPORT timeout",              test_report_timeout },
        { "touch during REPORT",         test_touch_during_report },
        { "report while LOCAL",          test_report_while_local },
        { "second report keeps since",   test_second_report_keeps_since },
        { "ms wraparound",               test_wraparound },
        { "snapshot complete",           test_snapshot_complete },
        { "bank active keeps LOCAL",     test_bank_active },
        { "default config",              test_defaults },
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

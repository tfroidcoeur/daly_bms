/*
 * Snapshot encoder tests. The golden files in docs/remote/fixtures/ are the
 * contract with the website plugin, so they are compared byte for byte; the
 * property checks below assert facts from the protocol directly, so a wrong
 * encoder cannot simply reproduce its own answers and pass.
 *
 * `test_snapshot --write-fixtures DIR` regenerates the golden files and exits.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "bms_model.h"
#include "snapshot_json.h"
#include "warnings.h"

int g_fail, g_checks;

#define SENTINEL 0x5A

static const snapshot_meta_t g_meta = { "3f9a0c27d1e84b56", "1.1.0", 41 };

/* Shared by encode and compare; big enough for every fixture. */
static char g_out[SNAPSHOT_JSON_MAX];
static char g_file[SNAPSHOT_JSON_MAX + 1];

/* ---- fixture models, from docs/remote/fixtures/README.md ----------------- */

typedef void (*model_builder_t)(system_model_t *m);

/* The base model: three healthy packs, pack p = 1..3 at Daly address p. */
static void build_base(system_model_t *m)
{
    bms_model_init(m);
    for (unsigned i = 0; i < BMS_PACK_COUNT; i++) {
        bms_pack_t *p = &m->pack[i];
        const unsigned n = i + 1;

        p->online = true;

        p->soc_valid   = true;
        p->pack_mv     = 83150 + 50 * (int32_t)n;
        p->pack_ma     = -4000;
        p->soc_pct_x10 = (uint16_t)(755 + 5 * n);

        p->charge_state    = 2;
        p->chg_mos         = true;
        p->dsg_mos         = true;
        p->charger_present = false;
        p->load_present    = true;

        p->cycles_valid = true;
        p->cycles       = (uint16_t)(11 + n);
        p->remaining_mah = 152000;

        p->cell_count = 24;
        for (unsigned c = 0; c < 24; c++) {
            p->cell_mv[c] = (uint16_t)(3465 + ((c + 1) % 5));   /* cell c+1 */
        }
        p->cells_valid   = true;
        p->cell_min_mv   = 3465;
        p->cell_min_idx  = 5;
        p->cell_max_mv   = 3469;
        p->cell_max_idx  = 4;

        p->temp_count = 2;
        p->temp_c[0]  = 18;
        p->temp_c[1]  = 19;
        p->temps_valid       = true;
        p->temp_minmax_valid = true;
        p->temp_min_c   = 18;
        p->temp_min_idx = 1;
        p->temp_max_c   = 19;
        p->temp_max_idx = 2;

        p->balance_bits = 0;
        /* alarms all zero, alarm_active false */
    }
}

static void build_normal(system_model_t *m)
{
    build_base(m);
}

static void build_weak_cell(system_model_t *m)
{
    build_base(m);
    bms_pack_t *p = &m->pack[1];            /* pack 2 */
    p->cell_mv[6]   = 3345;                 /* cell 7 */
    p->cell_min_mv  = 3345;
    p->cell_min_idx = 7;
    p->balance_bits = (1u << 0) | (1u << 3);   /* cells 1 and 4 */
    p->alarms[0]    = 0x01;
    p->alarm_active = true;
}

static void build_offline(system_model_t *m)
{
    build_base(m);
    m->pack[2].online = false;              /* pack 3 */
    m->pack[0].cycles_valid = false;        /* pack 1 */
    m->pack[0].temps_valid  = false;
}

static void build_all_offline(system_model_t *m)
{
    build_base(m);
    for (unsigned i = 0; i < BMS_PACK_COUNT; i++) {
        m->pack[i].online = false;
    }
}

static const struct {
    const char     *file;
    model_builder_t build;
} g_fixtures[] = {
    { "normal.json",      build_normal },
    { "weak-cell.json",   build_weak_cell },
    { "offline.json",     build_offline },
    { "all-offline.json", build_all_offline },
};
#define N_FIXTURES (sizeof g_fixtures / sizeof g_fixtures[0])

/* Build, evaluate the warnings as the firmware does, and encode. */
static size_t encode_model(model_builder_t build, char *buf, size_t cap)
{
    system_model_t m;
    warning_set_t  w;

    build(&m);
    warnings_evaluate(&m, &w);
    return snapshot_json_encode(&m, &w, &g_meta, buf, cap);
}

static void expect_has(const char *json, const char *needle)
{
    g_checks++;
    if (strstr(json, needle) == NULL) {
        g_fail++;
        printf("  FAIL missing: %s\n", needle);
    }
}

/* ---- tests ----------------------------------------------------------------- */

static void test_builders(void)
{
    system_model_t m;
    warning_set_t  w;
    bms_summary_t  s;

    build_normal(&m);
    s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 3);
    CHECK_EQ(s.bank_mv, 83250);
    CHECK_EQ(s.total_ma, -12000);
    CHECK_EQ(bms_summary_watts(&s), -999);
    CHECK_EQ(s.mean_soc_pct_x10, 765);
    CHECK_EQ(s.worst_cell_delta_mv, 4);
    warnings_evaluate(&m, &w);
    CHECK_EQ(w.count, 0);

    build_weak_cell(&m);
    warnings_evaluate(&m, &w);
    CHECK_EQ(w.count, 2);            /* cell spread 124 mV, and the BMS fault */

    build_offline(&m);
    s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 2);
    warnings_evaluate(&m, &w);
    CHECK_EQ(w.count, 1);            /* pack 3 not responding */

    build_all_offline(&m);
    s = bms_model_summary(&m);
    CHECK_EQ(s.online_count, 0);
    warnings_evaluate(&m, &w);
    CHECK_EQ(w.count, 3);
}

static void test_golden_match(void)
{
    for (unsigned f = 0; f < N_FIXTURES; f++) {
        char path[512];
        FILE *fp;
        size_t flen, n;

        snprintf(path, sizeof path, "%s/%s", FIXTURE_DIR, g_fixtures[f].file);
        fp = fopen(path, "rb");
        CHECK(fp != NULL);
        if (fp == NULL) {
            printf("  cannot open %s\n", path);
            continue;
        }
        flen = fread(g_file, 1, sizeof g_file, fp);
        fclose(fp);

        n = encode_model(g_fixtures[f].build, g_out, sizeof g_out);
        CHECK(n > 0);
        CHECK_EQ(n, flen);
        if (n != flen || memcmp(g_out, g_file, n) != 0) {
            printf("  mismatch in %s\n", g_fixtures[f].file);
        }
    }
}

static void test_property_normal(void)
{
    const size_t n = encode_model(build_normal, g_out, sizeof g_out);
    CHECK(n > 0);
    expect_has(g_out, "\"schema\":1,\"req\":\"3f9a0c27d1e84b56\",\"fw\":\"1.1.0\",\"uptime_s\":41");
    expect_has(g_out, "{\"addr\":1,\"online\":true,\"soc_x10\":760,\"mv\":83200,\"ma\":-4000,");
    expect_has(g_out, "\"cells_mv\":[3466,3467,3468,3469,3465,");
    expect_has(g_out, "\"warnings\":[]");
}

static void test_property_offline(void)
{
    const size_t n = encode_model(build_offline, g_out, sizeof g_out);
    CHECK(n > 0);
    expect_has(g_out, "{\"addr\":3,\"online\":false}");
    expect_has(g_out, "\"code\":\"pack_offline\",\"level\":\"alarm\",\"pack\":3");
    expect_has(g_out, "\"cycles\":null");
    expect_has(g_out, "\"temps_c\":null");
}

static void test_property_weak_cell(void)
{
    const size_t n = encode_model(build_weak_cell, g_out, sizeof g_out);
    CHECK(n > 0);
    expect_has(g_out, "\"balancing\":[1,4]");
    expect_has(g_out, "\"faults\":[{\"pos\":0,");
}

static void test_property_all_offline(void)
{
    const size_t n = encode_model(build_all_offline, g_out, sizeof g_out);
    CHECK(n > 0);
    expect_has(g_out, "\"bank\":{\"online\":0,\"mv\":null,\"ma\":null,\"w\":null,\"soc_x10\":null");
}

/* Every cap below the length must fail cleanly, and nothing past cap may move. */
static void test_buffer_too_small(void)
{
    static char guard[SNAPSHOT_JSON_MAX + 64];

    for (unsigned f = 0; f < N_FIXTURES; f++) {
        const size_t full = encode_model(g_fixtures[f].build, g_out, sizeof g_out);
        CHECK(full > 0);

        for (size_t cap = 0; cap <= full; cap++) {
            size_t n;
            memset(guard, SENTINEL, sizeof guard);
            n = encode_model(g_fixtures[f].build, guard, cap);
            CHECK_EQ(n, 0);
            if (cap > 0) {
                CHECK_EQ(guard[0], '\0');
            }
            for (size_t i = cap; i < sizeof guard; i++) {
                if ((unsigned char)guard[i] != SENTINEL) {
                    g_fail++;
                    printf("  FAIL %s wrote past cap %lu at %lu\n",
                           g_fixtures[f].file, (unsigned long)cap,
                           (unsigned long)i);
                    break;
                }
            }
        }

        /* Exactly one byte more than the text, for its NUL, is enough. */
        memset(guard, SENTINEL, sizeof guard);
        CHECK_EQ(encode_model(g_fixtures[f].build, guard, full + 1), full);
        CHECK(memcmp(guard, g_out, full) == 0);
        CHECK_EQ(guard[full], '\0');
    }
}

static void test_bad_req(void)
{
    static const char *const bad[] = {
        "3F9A0C27D1E84B56",   /* uppercase */
        "3f9a0c27d1e84b5",    /* 15 characters */
        "3f9a0c27d1e84b567",  /* 17 characters */
        "3f9a0c27d1e84b5g",   /* not hex */
    };
    system_model_t m;
    warning_set_t  w;
    snapshot_meta_t meta = g_meta;

    build_normal(&m);
    warnings_evaluate(&m, &w);

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char buf[SNAPSHOT_JSON_MAX];
        meta.req = bad[i];
        memset(buf, SENTINEL, sizeof buf);
        CHECK_EQ(snapshot_json_encode(&m, &w, &meta, buf, sizeof buf), 0);
        CHECK_EQ(buf[0], '\0');
    }

    meta.req = NULL;
    memset(g_out, SENTINEL, sizeof g_out);
    CHECK_EQ(snapshot_json_encode(&m, &w, &meta, g_out, sizeof g_out), 0);
    CHECK_EQ(g_out[0], '\0');
}

static void test_clean_text(void)
{
    char out[SNAPSHOT_TEXT_MAX + 1];
    char longin[101];

    /* a < b > " c " \ and the two bytes of a UTF-8 e-acute: one space each */
    snapshot_clean_text("a<b>\"c\"\\\xc3\xa9", out);
    CHECK(strcmp(out, "a b  c    ") == 0);

    snapshot_clean_text("tab\there", out);
    CHECK(strcmp(out, "tab here") == 0);

    memset(longin, 'x', sizeof longin - 1);
    longin[sizeof longin - 1] = '\0';
    snapshot_clean_text(longin, out);
    CHECK_EQ(strlen(out), SNAPSHOT_TEXT_MAX);
    CHECK(strspn(out, "x") == SNAPSHOT_TEXT_MAX);

    snapshot_clean_text("AZaz09 .,:%/()+-", out);
    CHECK(strcmp(out, "AZaz09 .,:%/()+-") == 0);
}

/*
 * Structure of a compact encoding: brackets balance, no space outside a string,
 * no backslash anywhere, and inside a string only the protocol's characters
 * (plus '_', which only the key names use) - so no quote can appear within a
 * text value.
 */
static void check_structure(const char *json, const char *name)
{
    size_t braces = 0, brackets = 0, quotes = 0;
    bool in_str = false;
    bool bad = false;

    for (const char *p = json; *p; p++) {
        const char c = *p;
        if (c == '{') braces++;
        if (c == '}') braces--;
        if (c == '[') brackets++;
        if (c == ']') brackets--;
        if (c == '\\') bad = true;
        if (c == '"') {
            quotes++;
            in_str = !in_str;
            continue;
        }
        if (in_str) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_' ||
                            strchr(" .,:%/()+-", c) != NULL;
            if (!ok) bad = true;
        } else if (c == ' ' || c == '\n') {
            bad = true;
        }
    }

    CHECK_EQ(braces, 0);
    CHECK_EQ(brackets, 0);
    CHECK(quotes % 2 == 0);
    CHECK(!in_str);
    if (bad || braces != 0 || brackets != 0 || quotes % 2 != 0 || in_str) {
        printf("  structure check failed for %s\n", name);
    }
}

static void test_structure(void)
{
    for (unsigned f = 0; f < N_FIXTURES; f++) {
        const size_t n = encode_model(g_fixtures[f].build, g_out, sizeof g_out);
        CHECK(n > 0);
        check_structure(g_out, g_fixtures[f].file);
    }
}

/* ---- --write-fixtures DIR ------------------------------------------------- */

static int write_fixtures(const char *dir)
{
    int failed = 0;

    for (unsigned f = 0; f < N_FIXTURES; f++) {
        char path[512];
        FILE *fp;
        const size_t n = encode_model(g_fixtures[f].build, g_out, sizeof g_out);

        if (n == 0) {
            fprintf(stderr, "encode failed for %s\n", g_fixtures[f].file);
            failed = 1;
            continue;
        }
        if (snprintf(path, sizeof path, "%s/%s", dir, g_fixtures[f].file)
                >= (int)sizeof path) {
            fprintf(stderr, "path too long: %s\n", dir);
            return 1;
        }
        fp = fopen(path, "wb");
        if (fp == NULL || fwrite(g_out, 1, n, fp) != n || fclose(fp) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            failed = 1;
            continue;
        }
        printf("wrote %s (%lu bytes)\n", path, (unsigned long)n);
    }
    return failed;
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "fixture models",              test_builders },
        { "golden fixtures, byte exact", test_golden_match },
        { "normal: independent facts",   test_property_normal },
        { "offline: independent facts",  test_property_offline },
        { "weak cell: independent facts", test_property_weak_cell },
        { "all offline: independent facts", test_property_all_offline },
        { "buffer too small",            test_buffer_too_small },
        { "bad request id",              test_bad_req },
        { "text cleaning",               test_clean_text },
        { "compact structure",           test_structure },
    };

    if (argc == 3 && strcmp(argv[1], "--write-fixtures") == 0) {
        return write_fixtures(argv[2]);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--write-fixtures DIR]\n", argv[0]);
        return 2;
    }

    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_fail;
        tests[i].fn();
        printf("%-32s %s\n", tests[i].name,
               g_fail == before ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

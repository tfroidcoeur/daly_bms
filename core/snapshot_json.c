#include "snapshot_json.h"

#include "daly_proto.h"

#include <stdio.h>
#include <string.h>

/*
 * Output goes through a bounded writer. Every append checks that its bytes and
 * the terminating NUL still fit; the first one that does not sets `overflow`
 * and nothing more is written. A short buffer therefore costs a failed encode,
 * never a write past `cap`, and never a truncated snapshot passed off as whole.
 */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;       /* always < cap while cap > 0 */
    bool    overflow;
} out_t;

static void put_n(out_t *o, const char *s, size_t n)
{
    if (o->overflow || n >= o->cap - o->len) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void put(out_t *o, const char *s)
{
    put_n(o, s, strlen(s));
}

/* For literals the protocol already allows: enum names, codes, levels. */
static void put_quoted(out_t *o, const char *s)
{
    put(o, "\"");
    put(o, s);
    put(o, "\"");
}

static void put_int(out_t *o, long long v)
{
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%lld", v);
    put(o, tmp);
}

static void put_bool(out_t *o, bool v)
{
    put(o, v ? "true" : "false");
}

/* A value the BMS has not reported yet is null, never zero. */
static void put_opt(out_t *o, bool valid, long long v)
{
    if (valid) {
        put_int(o, v);
    } else {
        put(o, "null");
    }
}

static bool text_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           (c != '\0' && strchr(" .,:%/()+-", c) != NULL);
}

/* Free text, cleaned to the protocol's character set and quoted. */
static void put_text(out_t *o, const char *src)
{
    char text[SNAPSHOT_TEXT_MAX + 1];
    snapshot_clean_text(src, text);
    put_quoted(o, text);
}

void snapshot_clean_text(const char *src, char dst[SNAPSHOT_TEXT_MAX + 1])
{
    size_t i = 0;
    for (; i < SNAPSHOT_TEXT_MAX && src[i] != '\0'; i++) {
        dst[i] = text_char_ok(src[i]) ? src[i] : ' ';
    }
    dst[i] = '\0';
}

/* The protocol names three states; anything else reads as idle. */
static const char *state_name(uint8_t s)
{
    switch (s) {
    case 1:  return "charging";
    case 2:  return "discharging";
    default: return "idle";
    }
}

/* Must match warn_code_t in declaration order, minus the WARN_ prefix. */
static const char *const k_warn_names[] = {
    "pack_offline",
    "pack_voltage_mismatch",
    "cell_count_mismatch",
    "cell_implausible",
    "cell_spread",
    "bms_fault",
    "soc_low",
    "temp_high",
    "temp_low",
    "charging_below_zero",
};

static const char *warn_name(warn_code_t code)
{
    const size_t n = sizeof k_warn_names / sizeof k_warn_names[0];
    return (size_t)code < n ? k_warn_names[code] : "unknown";
}

/* Only a request id of exactly 16 lowercase hex characters is accepted. */
static bool req_ok(const char *req)
{
    if (req == NULL || strlen(req) != SNAPSHOT_REQ_LEN) {
        return false;
    }
    for (size_t i = 0; i < SNAPSHOT_REQ_LEN; i++) {
        const char c = req[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void put_bank(out_t *o, const system_model_t *m)
{
    /* Recomputed on every call; cheap, and nothing here is cached. */
    const bms_summary_t s = bms_model_summary(m);
    const bool any = s.online_count > 0;

    put(o, ",\"bank\":{\"online\":");
    put_int(o, s.online_count);
    put(o, ",\"mv\":");
    put_opt(o, any, s.bank_mv);
    put(o, ",\"ma\":");
    put_opt(o, any, s.total_ma);
    put(o, ",\"w\":");
    put_opt(o, any, bms_summary_watts(&s));
    put(o, ",\"soc_x10\":");
    put_opt(o, any, s.mean_soc_pct_x10);
    put(o, ",\"state\":");
    put_quoted(o, state_name(s.charge_state));
    put(o, ",\"charger\":");
    put_bool(o, s.charger_present);
    put(o, ",\"cell_delta_mv\":");
    put_int(o, s.worst_cell_delta_mv);
    put(o, "}");
}

static void put_cells(out_t *o, const bms_pack_t *p)
{
    if (!p->cells_valid || p->cell_count == 0) {
        put(o, "null");
        return;
    }
    const unsigned n = p->cell_count > BMS_MAX_CELLS ? BMS_MAX_CELLS
                                                     : p->cell_count;
    put(o, "[");
    for (unsigned i = 0; i < n; i++) {
        if (i) put(o, ",");
        put_int(o, p->cell_mv[i]);
    }
    put(o, "]");
}

static void put_temps(out_t *o, const bms_pack_t *p)
{
    if (!p->temps_valid || p->temp_count == 0) {
        put(o, "null");
        return;
    }
    const unsigned n = p->temp_count > BMS_MAX_TEMPS ? BMS_MAX_TEMPS
                                                     : p->temp_count;
    put(o, "[");
    for (unsigned i = 0; i < n; i++) {
        if (i) put(o, ",");
        put_int(o, p->temp_c[i]);
    }
    put(o, "]");
}

/* 1-based numbers of the cells whose balance bit is set, up to cell_count. */
static void put_balancing(out_t *o, const bms_pack_t *p)
{
    unsigned n = p->cell_count ? p->cell_count : BMS_MAX_CELLS;
    if (n > BMS_MAX_CELLS) {
        n = BMS_MAX_CELLS;
    }
    unsigned listed = 0;
    put(o, "[");
    for (unsigned i = 0; i < n; i++) {
        if ((p->balance_bits >> i) & 1u) {
            if (listed++) put(o, ",");
            put_int(o, i + 1);
        }
    }
    put(o, "]");
}

/* Every set 0x98 bit in wire order, each with its own text. */
static void put_faults(out_t *o, const bms_pack_t *p)
{
    unsigned listed = 0;
    char text[64];

    put(o, "[");
    for (unsigned byte = 0; byte < BMS_ALARM_BYTES; byte++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            if (!((p->alarms[byte] >> bit) & 1u)) {
                continue;
            }
            const uint8_t pos = (uint8_t)(byte * 8 + bit);
            daly_fault_describe(pos, text, sizeof text);
            if (listed++) put(o, ",");
            put(o, "{\"pos\":");
            put_int(o, pos);
            put(o, ",\"trip\":");
            put_bool(o, daly_fault_is_trip((uint8_t)byte, (uint8_t)bit));
            put(o, ",\"text\":");
            put_text(o, text);
            put(o, "}");
        }
    }
    put(o, "]");
}

static void put_pack(out_t *o, const bms_pack_t *p)
{
    put(o, "{\"addr\":");
    put_int(o, p->addr);
    if (!p->online) {
        put(o, ",\"online\":false}");
        return;
    }
    put(o, ",\"online\":true");

    /* Voltage, current, SoC and power all come from 0x90: together or not at all. */
    put(o, ",\"soc_x10\":");
    put_opt(o, p->soc_valid, p->soc_pct_x10);
    put(o, ",\"mv\":");
    put_opt(o, p->soc_valid, p->pack_mv);
    put(o, ",\"ma\":");
    put_opt(o, p->soc_valid, p->pack_ma);
    put(o, ",\"w\":");
    put_opt(o, p->soc_valid, bms_pack_watts(p));

    put(o, ",\"state\":");
    put_quoted(o, state_name(p->charge_state));
    put(o, ",\"chg_mos\":");
    put_bool(o, p->chg_mos);
    put(o, ",\"dsg_mos\":");
    put_bool(o, p->dsg_mos);

    put(o, ",\"cycles\":");
    put_opt(o, p->cycles_valid, p->cycles);
    put(o, ",\"remaining_mah\":");
    put_int(o, p->remaining_mah);

    /* Both zero means 0x91 has not landed yet. */
    const bool minmax = p->cell_min_mv || p->cell_max_mv;
    put(o, ",\"cell_min_mv\":");
    put_opt(o, minmax, p->cell_min_mv);
    put(o, ",\"cell_max_mv\":");
    put_opt(o, minmax, p->cell_max_mv);

    put(o, ",\"temp_min_c\":");
    put_opt(o, p->temp_minmax_valid, p->temp_min_c);
    put(o, ",\"temp_max_c\":");
    put_opt(o, p->temp_minmax_valid, p->temp_max_c);

    put(o, ",\"cells_mv\":");
    put_cells(o, p);
    put(o, ",\"temps_c\":");
    put_temps(o, p);

    put(o, ",\"balancing\":");
    put_balancing(o, p);
    put(o, ",\"faults\":");
    put_faults(o, p);
    put(o, "}");
}

static void put_warnings(out_t *o, const warning_set_t *w)
{
    put(o, "[");
    for (unsigned i = 0; i < w->count; i++) {
        const warning_t *x = &w->item[i];
        char text[96];

        if (i) put(o, ",");
        warnings_format(x, text, sizeof text);
        put(o, "{\"code\":");
        put_quoted(o, warn_name(x->code));
        put(o, ",\"level\":");
        put_quoted(o, x->level == WARN_LEVEL_ALARM ? "alarm" : "warn");
        put(o, ",\"pack\":");
        put_int(o, x->pack);
        put(o, ",\"text\":");
        put_text(o, text);
        put(o, "}");
    }
    put(o, "]");
}

static void put_snapshot(out_t *o, const system_model_t *m,
                         const warning_set_t *w, const snapshot_meta_t *meta)
{
    put(o, "{\"schema\":");
    put_int(o, SNAPSHOT_SCHEMA);
    put(o, ",\"req\":");
    put_text(o, meta->req);
    put(o, ",\"fw\":");
    put_text(o, meta->fw);
    put(o, ",\"uptime_s\":");
    put_int(o, meta->uptime_s);

    put_bank(o, m);

    put(o, ",\"packs\":[");
    for (unsigned i = 0; i < BMS_PACK_COUNT; i++) {
        if (i) put(o, ",");
        put_pack(o, &m->pack[i]);
    }
    put(o, "]");

    put(o, ",\"warnings\":");
    put_warnings(o, w);
    put(o, "}");
}

size_t snapshot_json_encode(const system_model_t *m, const warning_set_t *w,
                            const snapshot_meta_t *meta, char *buf, size_t cap)
{
    out_t o = { buf, cap, 0, false };
    bool ok = meta != NULL && req_ok(meta->req) && meta->fw != NULL;

    if (ok) {
        put_snapshot(&o, m, w, meta);
        ok = !o.overflow;
    }
    if (!ok) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    buf[o.len] = '\0';   /* len < cap, so this is inside the buffer */
    return o.len;
}

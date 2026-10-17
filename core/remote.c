#include "remote.h"

#include <stdio.h>
#include <string.h>

/* A report counts against the daily cap for this long. */
#define REPORT_WINDOW_S      86400u

/* Back-off doubles per failure. Past 16 failures the delay is capped anyway,
 * and clamping here keeps the shift well inside 32 bits. */
#define BACKOFF_FAILURES_MAX 16u

#define CHECKIN_PATH         "/device/checkin"
#define REPORT_PATH          "/device/report"

/* ---- reply parser ------------------------------------------------------- */

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static size_t skip_ws(const char *b, size_t len, size_t i)
{
    while (i < len && is_ws(b[i])) {
        i++;
    }
    return i;
}

/* A request id is exactly 16 lowercase hex characters. */
static bool req_is_hex(const char *p)
{
    for (size_t i = 0; i < REMOTE_REQ_LEN; i++) {
        const char c = p[i];
        if (!is_digit(c) && !(c >= 'a' && c <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool key_is(const char *k, size_t klen, const char *want)
{
    const size_t n = strlen(want);
    return klen == n && memcmp(k, want, n) == 0;
}

/* Reads the string at b[*i], which must start with '"'. No escapes exist in
 * the protocol, so a backslash is an error rather than something to decode.
 * On success start and slen give the contents and *i moves past the quote. */
static bool read_string(const char *b, size_t len, size_t *i,
                        const char **start, size_t *slen)
{
    size_t j;

    if (*i >= len || b[*i] != '"') {
        return false;
    }
    j = *i + 1;
    *start = b + j;
    while (j < len && b[j] != '"') {
        if (b[j] == '\\') {
            return false;
        }
        j++;
    }
    if (j >= len) {
        return false;
    }
    *slen = j - (*i + 1);
    *i = j + 1;
    return true;
}

/* Reads a bare non-negative integer at b[*i], 0 .. REMOTE_NEXT_S_MAX. No sign,
 * no fraction, no exponent, no leading zeros: anything else stops the number
 * early and the caller's separator check then rejects it. */
static bool read_next_s(const char *b, size_t len, size_t *i, uint32_t *out)
{
    size_t j = *i;
    size_t digits = 0;
    uint64_t v = 0;

    if (j >= len || !is_digit(b[j])) {
        return false;
    }
    if (b[j] == '0' && j + 1 < len && is_digit(b[j + 1])) {
        return false;
    }
    while (j < len && is_digit(b[j])) {
        /* Ten digits fit in uint64_t; an eleventh is out of range anyway. */
        if (digits == 10) {
            return false;
        }
        v = v * 10u + (uint64_t)(b[j] - '0');
        digits++;
        j++;
    }
    if (v > REMOTE_NEXT_S_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    *i = j;
    return true;
}

static bool parse_body(const char *b, size_t len, remote_reply_t *r)
{
    bool seen_cmd = false;
    bool seen_next = false;
    bool seen_req = false;
    size_t i;

    if (b == NULL || len == 0 || len > REMOTE_REPLY_MAX) {
        return false;
    }

    i = skip_ws(b, len, 0);
    if (i >= len || b[i] != '{') {
        return false;
    }
    i++;

    for (;;) {
        const char *key;
        size_t klen;

        i = skip_ws(b, len, i);
        if (!read_string(b, len, &i, &key, &klen)) {
            return false;
        }
        i = skip_ws(b, len, i);
        if (i >= len || b[i] != ':') {
            return false;
        }
        i = skip_ws(b, len, i + 1);

        if (key_is(key, klen, "cmd")) {
            const char *v;
            size_t vlen;

            if (seen_cmd || !read_string(b, len, &i, &v, &vlen)) {
                return false;
            }
            if (vlen == 4 && memcmp(v, "none", 4) == 0) {
                r->cmd = REMOTE_CMD_NONE;
            } else if (vlen == 6 && memcmp(v, "report", 6) == 0) {
                r->cmd = REMOTE_CMD_REPORT;
            } else {
                return false;
            }
            seen_cmd = true;
        } else if (key_is(key, klen, "next_s")) {
            if (seen_next || !read_next_s(b, len, &i, &r->next_s)) {
                return false;
            }
            seen_next = true;
        } else if (key_is(key, klen, "req")) {
            const char *v;
            size_t vlen;

            if (seen_req || !read_string(b, len, &i, &v, &vlen)) {
                return false;
            }
            if (vlen != REMOTE_REQ_LEN || !req_is_hex(v)) {
                return false;
            }
            memcpy(r->req, v, REMOTE_REQ_LEN);
            r->req[REMOTE_REQ_LEN] = '\0';
            seen_req = true;
        } else {
            return false;
        }

        i = skip_ws(b, len, i);
        if (i >= len) {
            return false;
        }
        if (b[i] == ',') {
            i++;
            continue;
        }
        if (b[i] == '}') {
            i++;
            break;
        }
        return false;
    }

    if (skip_ws(b, len, i) != len) {
        return false;
    }
    if (!seen_cmd || !seen_next) {
        return false;
    }
    /* req goes with report and only with report. */
    if ((r->cmd == REMOTE_CMD_REPORT) != seen_req) {
        return false;
    }
    return true;
}

bool remote_parse_reply(const char *body, size_t len, remote_reply_t *out)
{
    remote_reply_t r;

    memset(&r, 0, sizeof r);
    if (!parse_body(body, len, &r)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = r;
    return true;
}

/* ---- state -------------------------------------------------------------- */

void remote_state_init(remote_state_t *s, uint32_t min_interval_s)
{
    memset(s, 0, sizeof *s);
    s->magic           = REMOTE_STATE_MAGIC;
    s->interval_s      = REMOTE_DEFAULT_NEXT_S;
    s->min_interval_s  = min_interval_s ? min_interval_s : 1u;
    s->checkin_due_now = true;
}

bool remote_state_valid(const remote_state_t *s)
{
    size_t n;

    if (s->magic != REMOTE_STATE_MAGIC) {
        return false;
    }
    if (s->min_interval_s < 1 || s->interval_s > REMOTE_NEXT_S_MAX) {
        return false;
    }
    if (s->report_count > REMOTE_REPORTS_PER_DAY ||
        s->report_head >= REMOTE_REPORTS_PER_DAY) {
        return false;
    }

    /* The NUL must sit inside req[]: find it within the first 17 bytes. */
    for (n = 0; n <= REMOTE_REQ_LEN && s->req[n] != '\0'; n++) {
    }
    if (n > REMOTE_REQ_LEN) {
        return false;
    }
    return n == 0 || (n == REMOTE_REQ_LEN && req_is_hex(s->req));
}

void remote_on_wake(remote_state_t *s, remote_wake_t wake, uint32_t now_s)
{
    /* A timer wake is the schedule doing its job; anything a person or the
     * CAN bus caused asks for a check-in right away. */
    (void)now_s;
    if (wake != REMOTE_WAKE_TIMER) {
        s->checkin_due_now = true;
    }
}

bool remote_checkin_due(const remote_state_t *s, uint32_t now_s)
{
    /* Wraparound-safe: the difference is read as signed, so a schedule that
     * wraps past UINT32_MAX still compares correctly. */
    return s->checkin_due_now ||
           (s->have_next && (int32_t)(now_s - s->next_checkin_s) >= 0);
}

uint32_t remote_seconds_until_checkin(const remote_state_t *s, uint32_t now_s)
{
    if (remote_checkin_due(s, now_s)) {
        return 0;
    }
    if (!s->have_next) {
        return UINT32_MAX;
    }
    return s->next_checkin_s - now_s;
}

bool remote_wait_is_deep(uint32_t seconds)
{
    return seconds >= REMOTE_DEEP_SLEEP_MIN_S;
}

/* ---- check-in ----------------------------------------------------------- */

/* Drops ring entries outside the window (they are the oldest, so the live
 * ones are the newest report_count entries), then takes a slot for a report at
 * now_s. False if the day's cap is already used. */
static bool report_slot_take(remote_state_t *s, uint32_t now_s)
{
    uint8_t live = 0;
    uint8_t k;

    for (k = 0; k < s->report_count; k++) {
        const uint8_t slot = (uint8_t)((s->report_head +
                                        REMOTE_REPORTS_PER_DAY -
                                        s->report_count + k) %
                                       REMOTE_REPORTS_PER_DAY);
        if (now_s - s->report_at_s[slot] < REPORT_WINDOW_S) {
            live++;
        }
    }
    s->report_count = live;

    if (live >= REMOTE_REPORTS_PER_DAY) {
        return false;
    }
    s->report_at_s[s->report_head] = now_s;
    s->report_head = (uint8_t)((s->report_head + 1) % REMOTE_REPORTS_PER_DAY);
    s->report_count = (uint8_t)(live + 1);
    return true;
}

/* min(interval, 15 min) doubled per failure, capped at 60 min. */
static uint32_t backoff_delay(uint32_t interval_s, uint8_t failures)
{
    const uint32_t f = failures > BACKOFF_FAILURES_MAX ? BACKOFF_FAILURES_MAX
                                                       : failures;
    const uint32_t base = interval_s < REMOTE_BACKOFF_BASE_S
                              ? interval_s
                              : REMOTE_BACKOFF_BASE_S;
    const uint32_t delay = base << (f - 1u);

    return delay > REMOTE_BACKOFF_CAP_S ? REMOTE_BACKOFF_CAP_S : delay;
}

remote_cmd_t remote_checkin(remote_state_t *s, const remote_transport_t *t,
                            const char *query, uint32_t now_s, uint32_t rand32,
                            bool *ok)
{
    char path[256];
    char resp[REMOTE_REPLY_MAX];
    size_t resp_len = 0;
    remote_reply_t reply;
    remote_cmd_t cmd = REMOTE_CMD_NONE;
    bool success = false;
    bool has_query = query != NULL && query[0] != '\0';
    int n;

    memset(&reply, 0, sizeof reply);
    s->checkin_due_now = false;

    n = has_query ? snprintf(path, sizeof path, CHECKIN_PATH "?%s", query)
                  : snprintf(path, sizeof path, CHECKIN_PATH);
    /* A path that does not fit is a failed check-in, not a truncated one. */
    if (n > 0 && (size_t)n < sizeof path) {
        const int status = t->request(t->ctx, "GET", path, NULL, 0,
                                      resp, sizeof resp, &resp_len);
        success = status == 200 && remote_parse_reply(resp, resp_len, &reply);
    }

    if (success) {
        s->failures = 0;
        s->interval_s = reply.next_s;
        if (s->interval_s != 0 && s->interval_s < s->min_interval_s) {
            s->interval_s = s->min_interval_s;
        }
        if (s->interval_s == 0) {
            s->have_next = false;
        } else {
            s->have_next = true;
            s->next_checkin_s = now_s + s->interval_s;
        }

        /* Over the cap, a report is treated as none. Either way an id that
         * is still being collected stays: with a short interval the next
         * check-in can arrive before the upload, and the server answers it
         * with none. */
        if (reply.cmd == REMOTE_CMD_REPORT && report_slot_take(s, now_s)) {
            memcpy(s->req, reply.req, sizeof s->req);
            cmd = REMOTE_CMD_REPORT;
        }
    } else {
        if (s->failures < UINT8_MAX) {
            s->failures++;
        }
        if (s->interval_s == 0) {
            s->have_next = false;
        } else {
            const uint32_t delay = backoff_delay(s->interval_s, s->failures);
            const uint32_t jitter = rand32 % (delay / 10u + 1u);

            s->have_next = true;
            s->next_checkin_s = now_s + delay + jitter;
        }
    }

    if (ok != NULL) {
        *ok = success;
    }
    return cmd;
}

int remote_upload(remote_state_t *s, const remote_transport_t *t,
                  const char *json, size_t len, uint32_t now_s)
{
    char resp[REMOTE_REPLY_MAX];
    size_t resp_len = 0;
    int status;

    /* Reports are not timed, so now_s plays no part in the upload. */
    (void)now_s;
    if (s->req[0] == '\0') {
        return REMOTE_ERR_TRANSPORT;
    }
    status = t->request(t->ctx, "POST", REPORT_PATH, json, len,
                        resp, sizeof resp, &resp_len);
    /* The protocol does not retry a report, so the request is spent either
     * way. */
    s->req[0] = '\0';
    return status;
}

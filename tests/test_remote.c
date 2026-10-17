/*
 * Host tests for core/remote.c: the reply parser, the check-in schedule,
 * back-off, the report cap, upload and wake handling. A fake transport
 * records what was sent and returns a scripted status and body.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "remote.h"

int g_fail, g_checks;

#define REQ_A "3f9a0c27d1e84b56"

/* ---- fake transport ----------------------------------------------------- */

typedef struct {
    int status;                 /* returned by the next call */
    const char *reply;          /* body copied into resp, NULL for none */
    size_t reply_len;
    int calls;
    char method[8];
    char path[256];
    char body[512];
    size_t body_len;
} fake_t;

static int fake_request(void *ctx, const char *method, const char *path,
                        const char *body, size_t body_len, char *resp,
                        size_t resp_cap, size_t *resp_len)
{
    fake_t *f = ctx;
    size_t n;

    f->calls++;
    snprintf(f->method, sizeof f->method, "%s", method);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->body_len = body ? body_len : 0;
    if (body) {
        n = body_len < sizeof f->body ? body_len : sizeof f->body;
        memcpy(f->body, body, n);
    }
    n = f->reply_len < resp_cap ? f->reply_len : resp_cap;
    if (n > 0) {
        memcpy(resp, f->reply, n);
    }
    *resp_len = n;
    return f->status;
}

static remote_transport_t transport_of(fake_t *f)
{
    remote_transport_t t = { fake_request, f };
    return t;
}

static void reply_with(fake_t *f, int status, const char *body)
{
    f->status = status;
    f->reply = body;
    f->reply_len = body ? strlen(body) : 0;
}

static remote_cmd_t checkin(remote_state_t *s, fake_t *f, uint32_t now,
                            uint32_t rand32, bool *ok)
{
    remote_transport_t t = transport_of(f);
    return remote_checkin(s, &t, NULL, now, rand32, ok);
}

static bool is_zero(const void *p, size_t n)
{
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) {
        if (c[i]) {
            return false;
        }
    }
    return true;
}

static bool parse_str(const char *body, remote_reply_t *r)
{
    return remote_parse_reply(body, strlen(body), r);
}

/* ---- parser ------------------------------------------------------------- */

static void test_parser_accepts(void)
{
    remote_reply_t r;

    CHECK(parse_str("{\"cmd\":\"none\",\"next_s\":900}", &r));
    CHECK(r.cmd == REMOTE_CMD_NONE);
    CHECK_EQ(r.next_s, 900);
    CHECK(r.req[0] == '\0');

    CHECK(parse_str("  {\n\t\"next_s\" : 900 ,\r\n  \"cmd\" : \"none\"\n}\n ", &r));
    CHECK(r.cmd == REMOTE_CMD_NONE);
    CHECK_EQ(r.next_s, 900);

    CHECK(parse_str("{\"cmd\":\"report\",\"next_s\":0,\"req\":\"" REQ_A "\"}", &r));
    CHECK(r.cmd == REMOTE_CMD_REPORT);
    CHECK_EQ(r.next_s, 0);
    CHECK(strcmp(r.req, REQ_A) == 0);

    CHECK(parse_str("{\"cmd\":\"none\",\"next_s\":1}", &r));
    CHECK_EQ(r.next_s, 1);
    CHECK(parse_str("{\"cmd\":\"none\",\"next_s\":1209600}", &r));
    CHECK_EQ(r.next_s, 1209600);
    CHECK(parse_str("{\"cmd\":\"none\",\"next_s\":0}", &r));
    CHECK_EQ(r.next_s, 0);

    /* 512 bytes is the largest reply: pad a valid one to exactly that. */
    {
        char big[REMOTE_REPLY_MAX];
        const char *json = "{\"cmd\":\"none\",\"next_s\":900}";
        memset(big, ' ', sizeof big);
        memcpy(big, json, strlen(json));
        CHECK(remote_parse_reply(big, REMOTE_REPLY_MAX, &r));
        CHECK_EQ(r.next_s, 900);
    }
}

static void expect_reject(const char *what, const char *body, size_t len)
{
    remote_reply_t r;

    g_checks++;
    if (remote_parse_reply(body, len, &r)) {
        g_fail++;
        printf("  FAIL %s:%d  accepted: %s\n", __FILE__, __LINE__, what);
    }
}

#define REJECT(s) expect_reject(#s, s, strlen(s))

static void test_parser_rejects(void)
{
    REJECT("{\"cmd\":\"none\",\"next_s\":1209601}");
    REJECT("{\"cmd\":\"none\",\"next_s\":-1}");
    REJECT("{\"cmd\":\"none\",\"next_s\":01}");
    REJECT("{\"cmd\":\"none\",\"next_s\":1.5}");
    REJECT("{\"cmd\":\"none\",\"next_s\":1e3}");
    REJECT("{\"cmd\":\"none\",\"next_s\":\"900\"}");
    REJECT("{\"cmd\":\"none\"}");
    REJECT("{\"next_s\":900}");
    REJECT("{\"cmd\":\"Report\",\"next_s\":900,\"req\":\"" REQ_A "\"}");
    REJECT("{\"cmd\":\"reboot\",\"next_s\":900}");
    REJECT("{\"cmd\":\"report\",\"next_s\":900}");
    REJECT("{\"cmd\":\"none\",\"next_s\":900,\"req\":\"" REQ_A "\"}");
    REJECT("{\"cmd\":\"report\",\"next_s\":900,\"req\":\"3F9A0C27D1E84B56\"}");
    REJECT("{\"cmd\":\"report\",\"next_s\":900,\"req\":\"3f9a0c27d1e84b5\"}");
    REJECT("{\"cmd\":\"report\",\"next_s\":900,\"req\":\"3f9a0c27d1e84b567\"}");
    /* One backslash in the JSON text: a 17-character string with an escape. */
    REJECT("{\"cmd\":\"report\",\"next_s\":900,\"req\":\"3f9a0c27d1e84\\b56\"}");
    REJECT("{\"cmd\":\"none\",\"cmd\":\"none\",\"next_s\":900}");
    REJECT("{\"cmd\":\"none\",\"next_s\":900,\"x\":1}");
    REJECT("{\"cmd\":\"none\",\"next_s\":900,}");
    REJECT("{\"cmd\":\"none\",\"next_s\":900}x");
    REJECT("");
    REJECT("{\"cmd\":\"none\",\"next_s\":12345678901234567890}");

    {
        char big[REMOTE_REPLY_MAX + 1];
        const char *json = "{\"cmd\":\"none\",\"next_s\":900}";
        memset(big, ' ', sizeof big);
        memcpy(big, json, strlen(json));
        expect_reject("513-byte padded reply", big, sizeof big);
    }
    {
        static const char nul_body[] = "{\"cmd\":\"none\",\0\"next_s\":900}";
        expect_reject("NUL in the middle", nul_body, sizeof nul_body - 1);
    }
}

static void test_parser_zeroes_out_on_reject(void)
{
    remote_reply_t r;

    memset(&r, 0xAA, sizeof r);
    CHECK(!parse_str("{\"cmd\":\"report\",\"next_s\":900}", &r));
    CHECK(is_zero(&r, sizeof r));

    memset(&r, 0xAA, sizeof r);
    CHECK(!remote_parse_reply(NULL, 0, &r));
    CHECK(is_zero(&r, sizeof r));
}

/* ---- check-in schedule -------------------------------------------------- */

static void test_checkin_schedules_next(void)
{
    fake_t f;
    remote_state_t s;
    bool ok = false;

    memset(&f, 0, sizeof f);
    remote_state_init(&s, 1);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":900}");

    CHECK(checkin(&s, &f, 1000, 0, &ok) == REMOTE_CMD_NONE);
    CHECK(ok);
    CHECK(!s.checkin_due_now);
    CHECK(s.have_next);
    CHECK_EQ(s.next_checkin_s, 1900);
    CHECK_EQ(remote_seconds_until_checkin(&s, 1000), 900);

    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":0}");
    CHECK(checkin(&s, &f, 2000, 0, &ok) == REMOTE_CMD_NONE);
    CHECK(ok);
    CHECK(!s.have_next);
    CHECK_EQ(remote_seconds_until_checkin(&s, 2000), UINT32_MAX);

    remote_state_init(&s, 300);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":10}");
    checkin(&s, &f, 1000, 0, NULL);
    CHECK_EQ(s.interval_s, 300);
    CHECK_EQ(s.next_checkin_s, 1300);
}

static void test_query_string(void)
{
    fake_t f;
    remote_state_t s;
    remote_transport_t t;
    char longq[300];

    memset(&f, 0, sizeof f);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":900}");
    t = transport_of(&f);

    remote_state_init(&s, 1);
    remote_checkin(&s, &t, "fw=1.1.0&state=standby", 0, 0, NULL);
    CHECK(strcmp(f.method, "GET") == 0);
    CHECK(strcmp(f.path, "/device/checkin?fw=1.1.0&state=standby") == 0);

    remote_checkin(&s, &t, NULL, 0, 0, NULL);
    CHECK(strcmp(f.path, "/device/checkin") == 0);

    remote_checkin(&s, &t, "", 0, 0, NULL);
    CHECK(strcmp(f.path, "/device/checkin") == 0);

    /* A path that does not fit fails without calling the transport. */
    memset(longq, 'a', sizeof longq - 1);
    longq[sizeof longq - 1] = '\0';
    f.calls = 0;
    {
        bool ok = true;
        remote_checkin(&s, &t, longq, 0, 0, &ok);
        CHECK_EQ(f.calls, 0);
        CHECK(!ok);
    }
}

/* Runs `n` failed check-ins, each at the moment the last one said to retry,
 * and checks the gap between each retry and the one before. */
static void expect_backoff(remote_state_t *s, fake_t *f, const uint32_t *want,
                           size_t n)
{
    uint32_t now = 1000;
    for (size_t i = 0; i < n; i++) {
        checkin(s, f, now, 0, NULL);
        CHECK_EQ(s->next_checkin_s - now, want[i]);
        now = s->next_checkin_s;
    }
}

static void test_checkin_failures(void)
{
    static const char *valid_report =
        "{\"cmd\":\"report\",\"next_s\":900,\"req\":\"" REQ_A "\"}";
    static const struct {
        int status;
        const char *reply;
    } cases[] = {
        { 500, NULL },
        { 302, NULL },
        { 401, NULL },
        { REMOTE_ERR_TRANSPORT, NULL },
        { 200, "{\"cmd\":\"none\"}" },
    };
    fake_t f;
    remote_state_t s;

    memset(&f, 0, sizeof f);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bool ok = true;
        remote_state_init(&s, 1);
        reply_with(&f, cases[i].status,
                   cases[i].reply ? cases[i].reply : valid_report);
        CHECK(checkin(&s, &f, 1000, 0, &ok) == REMOTE_CMD_NONE);
        CHECK(!ok);
        CHECK_EQ(s.failures, 1);
    }
}

static void test_backoff(void)
{
    static const uint32_t slow[] = { 900, 1800, 3600, 3600 };
    static const uint32_t fast[] = { 60, 120, 240, 480, 960, 1920, 3600, 3600 };
    fake_t f;
    remote_state_t s;
    uint32_t d;

    memset(&f, 0, sizeof f);

    /* Interval 900 (the default), no jitter. */
    remote_state_init(&s, 1);
    reply_with(&f, 500, NULL);
    expect_backoff(&s, &f, slow, sizeof slow / sizeof slow[0]);

    /* Interval 60 from the server. */
    remote_state_init(&s, 1);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":60}");
    checkin(&s, &f, 0, 0, NULL);
    reply_with(&f, 500, NULL);
    expect_backoff(&s, &f, fast, sizeof fast / sizeof fast[0]);

    /* Maximum jitter stays within a tenth of the delay. */
    remote_state_init(&s, 1);
    reply_with(&f, 500, NULL);
    checkin(&s, &f, 1000, UINT32_MAX, NULL);
    d = s.next_checkin_s - 1000;
    CHECK(d >= 900 && d <= 990);

    /* The jitter is rand32 modulo delay/10 + 1: exact values pin the bound. */
    {
        static const uint32_t rnd[][2] = { { 0, 900 }, { 45, 945 }, { 90, 990 },
                                           { 91, 900 }, { 181, 990 } };
        for (unsigned k = 0; k < sizeof rnd / sizeof rnd[0]; k++) {
            remote_state_init(&s, 1);
            reply_with(&f, 500, NULL);
            checkin(&s, &f, 1000, rnd[k][0], NULL);
            CHECK_EQ(s.next_checkin_s - 1000, rnd[k][1]);
        }
    }

    /* A success resets the back-off. */
    remote_state_init(&s, 1);
    reply_with(&f, 500, NULL);
    checkin(&s, &f, 0, 0, NULL);
    checkin(&s, &f, 900, 0, NULL);
    CHECK_EQ(s.failures, 2);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":900}");
    checkin(&s, &f, 5000, 0, NULL);
    CHECK_EQ(s.failures, 0);
    CHECK_EQ(s.next_checkin_s, 5900);
    reply_with(&f, 500, NULL);
    checkin(&s, &f, 5900, 0, NULL);
    CHECK_EQ(s.next_checkin_s - 5900, 900);

    /* Disabled timed check-ins: a failure is not retried. */
    remote_state_init(&s, 1);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":0}");
    checkin(&s, &f, 0, 0, NULL);
    reply_with(&f, 500, NULL);
    checkin(&s, &f, 10, 0, NULL);
    CHECK_EQ(remote_seconds_until_checkin(&s, 10), UINT32_MAX);
}

/* ---- report cap --------------------------------------------------------- */

static void test_report_cap(void)
{
    fake_t f;
    remote_state_t s;
    int i;

    memset(&f, 0, sizeof f);
    remote_state_init(&s, 1);
    reply_with(&f, 200,
               "{\"cmd\":\"report\",\"next_s\":900,\"req\":\"0123456789abcdef\"}");

    for (i = 0; i < REMOTE_REPORTS_PER_DAY; i++) {
        CHECK(checkin(&s, &f, 1000 + (uint32_t)i, 0, NULL) == REMOTE_CMD_REPORT);
    }
    s.req[0] = '\0';   /* as after the last upload */
    CHECK(checkin(&s, &f, 1100, 0, NULL) == REMOTE_CMD_NONE);
    CHECK(s.req[0] == '\0');
    CHECK_EQ(s.report_count, REMOTE_REPORTS_PER_DAY);

    /* The first report is now a day old: one slot is free again. */
    CHECK(checkin(&s, &f, 1000 + 86400, 0, NULL) == REMOTE_CMD_REPORT);
    CHECK(strcmp(s.req, "0123456789abcdef") == 0);
}

/* ---- upload ------------------------------------------------------------- */

static void test_upload(void)
{
    static const char *json = "{\"schema\":1,\"req\":\"" REQ_A "\"}";
    static const char *report_reply =
        "{\"cmd\":\"report\",\"next_s\":900,\"req\":\"" REQ_A "\"}";
    fake_t f;
    remote_state_t s;
    remote_transport_t t;
    int calls;

    memset(&f, 0, sizeof f);
    t = transport_of(&f);
    remote_state_init(&s, 1);

    reply_with(&f, 200, report_reply);
    CHECK(checkin(&s, &f, 1000, 0, NULL) == REMOTE_CMD_REPORT);
    reply_with(&f, 204, NULL);
    CHECK_EQ(remote_upload(&s, &t, json, strlen(json), 1001), 204);
    CHECK(strcmp(f.method, "POST") == 0);
    CHECK(strcmp(f.path, "/device/report") == 0);
    CHECK_EQ(f.body_len, strlen(json));
    CHECK(memcmp(f.body, json, strlen(json)) == 0);
    CHECK(s.req[0] == '\0');

    /* The request is spent even when the server fails. */
    reply_with(&f, 200, report_reply);
    CHECK(checkin(&s, &f, 1100, 0, NULL) == REMOTE_CMD_REPORT);
    reply_with(&f, 500, NULL);
    CHECK_EQ(remote_upload(&s, &t, json, strlen(json), 1101), 500);
    CHECK(s.req[0] == '\0');

    /* A check-in that answers none while a report is still being collected
     * (short interval) must not throw the request away. */
    reply_with(&f, 200, report_reply);
    CHECK(checkin(&s, &f, 1200, 0, NULL) == REMOTE_CMD_REPORT);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":1}");
    CHECK(checkin(&s, &f, 1201, 0, NULL) == REMOTE_CMD_NONE);
    CHECK(s.req[0] != '\0');
    reply_with(&f, 204, NULL);
    CHECK_EQ(remote_upload(&s, &t, json, strlen(json), 1202), 204);

    /* Nothing outstanding: no call at all. */
    calls = f.calls;
    CHECK_EQ(remote_upload(&s, &t, json, strlen(json), 1102), REMOTE_ERR_TRANSPORT);
    CHECK_EQ(f.calls, calls);
}

/* ---- wake and due ------------------------------------------------------- */

static void test_wake(void)
{
    fake_t f;
    remote_state_t s;

    memset(&f, 0, sizeof f);
    remote_state_init(&s, 1);
    reply_with(&f, 200, "{\"cmd\":\"none\",\"next_s\":0}");

    /* Disabled, with nothing due: only a local wake brings a check-in. */
    checkin(&s, &f, 1000, 0, NULL);
    CHECK(!remote_checkin_due(&s, 5000));
    remote_on_wake(&s, REMOTE_WAKE_TIMER, 5000);
    CHECK(!remote_checkin_due(&s, 5000));

    remote_on_wake(&s, REMOTE_WAKE_TOUCH, 5000);
    CHECK(remote_checkin_due(&s, 5000));
    CHECK_EQ(remote_seconds_until_checkin(&s, 5000), 0);
    checkin(&s, &f, 5000, 0, NULL);
    CHECK(!remote_checkin_due(&s, 5000));

    remote_on_wake(&s, REMOTE_WAKE_CAN, 6000);
    CHECK(remote_checkin_due(&s, 6000));
    checkin(&s, &f, 6000, 0, NULL);

    remote_on_wake(&s, REMOTE_WAKE_BOOT, 7000);
    CHECK(remote_checkin_due(&s, 7000));
    checkin(&s, &f, 7000, 0, NULL);

    /* A schedule near UINT32_MAX that wraps past 0 is still compared right. */
    s.checkin_due_now = false;
    s.have_next = true;
    s.next_checkin_s = UINT32_MAX - 100u;
    CHECK(remote_checkin_due(&s, 50));
    CHECK(!remote_checkin_due(&s, UINT32_MAX - 200u));
    CHECK_EQ(remote_seconds_until_checkin(&s, UINT32_MAX - 200u), 100);
}

/* ---- state validity and deep sleep -------------------------------------- */

static void test_state_valid(void)
{
    remote_state_t z;
    remote_state_t s;

    memset(&z, 0, sizeof z);
    CHECK(!remote_state_valid(&z));

    remote_state_init(&s, 1);
    CHECK(remote_state_valid(&s));

    memcpy(s.req, REQ_A, sizeof REQ_A);
    CHECK(remote_state_valid(&s));

    s.req[3] = 'Z';
    CHECK(!remote_state_valid(&s));

    memset(s.req, 'a', sizeof s.req);
    CHECK(!remote_state_valid(&s));

    remote_state_init(&s, 1);
    s.report_count = REMOTE_REPORTS_PER_DAY + 1;
    CHECK(!remote_state_valid(&s));
}

static void test_wait_is_deep(void)
{
    CHECK(!remote_wait_is_deep(59));
    CHECK(remote_wait_is_deep(60));
    CHECK(remote_wait_is_deep(UINT32_MAX));
}

/* ---- runner ------------------------------------------------------------- */

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "parser: accepts",             test_parser_accepts },
        { "parser: rejects",             test_parser_rejects },
        { "parser: zeroes out",          test_parser_zeroes_out_on_reject },
        { "checkin: schedules next",     test_checkin_schedules_next },
        { "checkin: query string",       test_query_string },
        { "checkin: failures",           test_checkin_failures },
        { "checkin: back-off",           test_backoff },
        { "report: daily cap",           test_report_cap },
        { "upload",                      test_upload },
        { "wake and due",                test_wake },
        { "state: valid",                test_state_valid },
        { "wait: deep sleep threshold",  test_wait_is_deep },
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

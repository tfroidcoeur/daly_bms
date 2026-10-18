/*
 * Talking to the club website: check in, and upload a snapshot when asked.
 *
 * The wire format and the rules are in docs/remote-protocol.md. This module is
 * the policy half - when to check in, what a reply means, back-off, the report
 * cap - and does no I/O of its own. The port supplies a transport (libcurl on
 * the host, esp_http_client on the board) and a clock.
 *
 * Time is `now_s`: monotonic seconds that keep counting across deep sleep (the
 * ESP32's RTC timer; CLOCK_MONOTONIC on the host). Nothing here needs the date.
 *
 * remote_state_t is plain data with no pointers, so the board can keep it in
 * RTC memory across deep sleep and check it with remote_state_valid() on wake.
 */
#ifndef REMOTE_H
#define REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMOTE_REQ_LEN          16          /* hex characters in a request id */
#define REMOTE_REPLY_MAX        512         /* longest check-in reply accepted */
#define REMOTE_NEXT_S_MAX       1209600u    /* 14 days */
#define REMOTE_DEFAULT_NEXT_S   900u        /* until the server says otherwise */
#define REMOTE_BACKOFF_BASE_S   900u        /* first retry at most 15 min out */
#define REMOTE_BACKOFF_CAP_S    3600u       /* never more than 60 min */
#define REMOTE_REPORTS_PER_DAY  24
#define REMOTE_DEEP_SLEEP_MIN_S 60u         /* shorter waits are spent awake */
#define REMOTE_LIVE_S_MAX       3600u       /* longest live-report interval */
#define REMOTE_LIVE_UNSET       UINT32_MAX  /* the server left it to the device */

/* What a transport call returns besides an HTTP status. */
#define REMOTE_ERR_TRANSPORT    (-1)        /* DNS, TCP, TLS, timeout */
#define REMOTE_ERR_TOO_LONG     (-2)        /* reply longer than resp_cap */

/*
 * One HTTPS exchange. `path` is relative to the API base and includes any
 * query string ("/device/checkin?fw=1.1.0&state=standby"). `body` is NULL for
 * a GET. The transport adds the bearer token, follows no redirects, and copies
 * at most `resp_cap` bytes of the reply into `resp` (not NUL-terminated),
 * setting `*resp_len`.
 *
 * Returns the HTTP status (a 3xx is returned as is - it is the caller's to
 * reject), or one of REMOTE_ERR_*.
 */
typedef struct {
    int (*request)(void *ctx, const char *method, const char *path,
                   const char *body, size_t body_len,
                   char *resp, size_t resp_cap, size_t *resp_len);
    void *ctx;
} remote_transport_t;

typedef enum {
    REMOTE_CMD_NONE = 0,
    REMOTE_CMD_REPORT,
} remote_cmd_t;

/* A parsed check-in reply. */
typedef struct {
    remote_cmd_t cmd;
    uint32_t     next_s;                     /* 0 = timed check-ins disabled */
    char         req[REMOTE_REQ_LEN + 1];    /* "" unless cmd is REPORT */
    uint32_t     live_s;                     /* 0 = off, or REMOTE_LIVE_UNSET */
} remote_reply_t;

typedef enum {
    REMOTE_WAKE_BOOT = 0,   /* power-on or reset */
    REMOTE_WAKE_TIMER,
    REMOTE_WAKE_TOUCH,
    REMOTE_WAKE_CAN,
} remote_wake_t;

#define REMOTE_STATE_MAGIC 0x52454d32u      /* "REM2" */

typedef struct {
    uint32_t magic;
    uint32_t interval_s;          /* last next_s the server sent; 0 = disabled */
    uint32_t min_interval_s;      /* console floor, >= 1 */
    bool     checkin_due_now;     /* a local wake asked for one right away */
    bool     have_next;           /* next_checkin_s is meaningful */
    uint32_t next_checkin_s;
    uint8_t  failures;            /* consecutive failed check-ins */
    char     req[REMOTE_REQ_LEN + 1];   /* outstanding report, "" if none */
    uint32_t report_at_s[REMOTE_REPORTS_PER_DAY];  /* ring of report times */
    uint8_t  report_count;        /* entries used in the ring, <= 24 */
    uint8_t  report_head;         /* next slot to overwrite */
    uint32_t live_s;              /* the server's live interval; 0 = off,
                                     REMOTE_LIVE_UNSET = the device's own */
} remote_state_t;

/*
 * Fresh state, as after power-on: interval REMOTE_DEFAULT_NEXT_S, floor
 * `min_interval_s` (0 is taken as 1), and a check-in due straight away.
 */
void remote_state_init(remote_state_t *s, uint32_t min_interval_s);

/* True if `s` looks like something remote_state_init() produced - false for
 * RTC memory after a cold boot or a firmware with another layout. */
bool remote_state_valid(const remote_state_t *s);

/* Note what woke the device. A touch or a CAN wake makes a check-in due now,
 * even with timed check-ins disabled. */
void remote_on_wake(remote_state_t *s, remote_wake_t wake, uint32_t now_s);

/* True if a check-in should happen at `now_s`. */
bool remote_checkin_due(const remote_state_t *s, uint32_t now_s);

/*
 * Seconds until the next timed check-in, 0 if one is due now, UINT32_MAX if
 * none is scheduled (disabled, or disabled and the last attempt failed).
 */
uint32_t remote_seconds_until_checkin(const remote_state_t *s, uint32_t now_s);

/* True if waiting `seconds` is worth a deep sleep (>= REMOTE_DEEP_SLEEP_MIN_S,
 * or UINT32_MAX: nothing to wait for but a local wake). */
bool remote_wait_is_deep(uint32_t seconds);

/*
 * Parse a check-in reply strictly (see the table in docs/remote-protocol.md).
 * Accepts exactly the keys cmd, next_s, req and the optional live_s (0 ..
 * REMOTE_LIVE_S_MAX; REMOTE_LIVE_UNSET in `out` when absent) in any order, with
 * optional whitespace; rejects unknown keys, duplicates, wrong types,
 * out-of-range numbers, a malformed req and anything over REMOTE_REPLY_MAX.
 * Returns false and leaves `out` zeroed on any of those.
 */
bool remote_parse_reply(const char *body, size_t len, remote_reply_t *out);

/*
 * Do one check-in now and update `s`: schedule the next one, apply back-off on
 * failure, apply the floor to next_s, and apply the 24-a-day cap (a REPORT
 * over the cap comes back as NONE). On REPORT the request id is kept in
 * `s->req` for remote_upload().
 *
 * `query` is the extra query string the port wants to send, without the
 * leading '&' ("fw=1.1.0&state=standby&wake=timer"), or NULL. `rand32` is any
 * random number, for back-off jitter.
 *
 * Returns REMOTE_CMD_REPORT if a report should be collected and uploaded now,
 * REMOTE_CMD_NONE otherwise (including on failure; `ok` says which, if given).
 */
remote_cmd_t remote_checkin(remote_state_t *s, const remote_transport_t *t,
                            const char *query, uint32_t now_s, uint32_t rand32,
                            bool *ok);

/*
 * Upload `json` (from snapshot_json_encode(), with `s->req` as its req) and
 * clear the outstanding request whatever happens - the protocol does not retry
 * a report. Returns the HTTP status or a REMOTE_ERR_* value; 204 is success.
 */
int remote_upload(remote_state_t *s, const remote_transport_t *t,
                  const char *json, size_t len, uint32_t now_s);

/*
 * Seconds between live reports while the screen is on: the server's value
 * from the last good check-in if it sent one, else `device_live_s` (the
 * console setting). 0 means no live reports.
 */
uint32_t remote_live_interval(const remote_state_t *s, uint32_t device_live_s);

/*
 * Send a live snapshot (snapshot_json_encode() with a NULL req) unasked:
 * POST /device/live?every=<interval_s>. Live reports do not count against the
 * report cap and are not retried. Returns the HTTP status or a REMOTE_ERR_*
 * value; 204 is success.
 */
int remote_live(const remote_transport_t *t, const char *json, size_t len,
                uint32_t interval_s);

#endif /* REMOTE_H */

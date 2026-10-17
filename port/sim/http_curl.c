#include "http_curl.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} sink_t;

static size_t on_body(char *data, size_t size, size_t n, void *user)
{
    sink_t *s = user;
    const size_t got = size * n;
    size_t take = got;
    if (take > s->cap - s->len) {
        take = s->cap - s->len;
        s->overflow = true;
    }
    memcpy(s->buf + s->len, data, take);
    s->len += take;
    return got;   /* keep reading so the connection closes cleanly */
}

static int request(void *ctx, const char *method, const char *path,
                   const char *body, size_t body_len,
                   char *resp, size_t resp_cap, size_t *resp_len)
{
    const http_curl_t *c = ctx;
    char url[512], auth[128];
    *resp_len = 0;
    if ((size_t)snprintf(url, sizeof url, "%s%s", c->base, path) >= sizeof url) {
        return REMOTE_ERR_TRANSPORT;
    }
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->token);

    CURL *h = curl_easy_init();
    if (!h) {
        return REMOTE_ERR_TRANSPORT;
    }
    struct curl_slist *hdr = curl_slist_append(NULL, auth);
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, "Expect:");   /* no 100-continue round trip */

    sink_t sink = { resp, resp_cap, 0, false };
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);   /* a 3xx is an error */
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);         /* the board's budget */
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(h);

    *resp_len = sink.len;
    if (rc != CURLE_OK) {
        return REMOTE_ERR_TRANSPORT;
    }
    if (sink.overflow) {
        return REMOTE_ERR_TOO_LONG;
    }
    return (int)status;
}

bool http_curl_init(http_curl_t *c, remote_transport_t *t, const char *base,
                    const char *token)
{
    /* Plain http carries the token in clear, so only to this machine (the
     * docker test site). */
    if (strncmp(base, "https://", 8) != 0 &&
        strncmp(base, "http://localhost", 16) != 0 &&
        strncmp(base, "http://127.0.0.1", 16) != 0) {
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (strlen(base) >= sizeof c->base || strlen(token) >= sizeof c->token) {
        return false;
    }
    strcpy(c->base, base);
    size_t n = strlen(c->base);
    while (n > 0 && c->base[n - 1] == '/') {
        c->base[--n] = '\0';
    }
    strcpy(c->token, token);
    t->request = request;
    t->ctx     = c;
    return true;
}

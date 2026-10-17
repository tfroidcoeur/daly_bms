/*
 * remote.c's transport on the host: libcurl against the WordPress plugin,
 * usually the docker test site (http://localhost:8080) - which is why the
 * host, unlike the board, also accepts plain http.
 */
#ifndef HTTP_CURL_H
#define HTTP_CURL_H

#include <stdbool.h>

#include "remote.h"

typedef struct {
    char base[256];     /* ".../wp-json/avia-bms/v1", no trailing slash */
    char token[80];
} http_curl_t;

/* Fill `t` to talk to `base` with `token`. False if either does not fit or
 * `base` is not an http(s) URL. */
bool http_curl_init(http_curl_t *c, remote_transport_t *t, const char *base,
                    const char *token);

#endif /* HTTP_CURL_H */

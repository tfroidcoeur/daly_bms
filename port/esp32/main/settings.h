/*
 * What the device needs to reach the website: Wi-Fi, server and token.
 *
 * Kept in NVS, set over the USB console (console.c), with Kconfig defaults
 * for a first flash. The secrets are write-only from the console's point of
 * view: settings_get() hands them to net.c, and nothing ever prints them.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_SSID_MAX   32
#define SETTINGS_PASS_MAX   64
#define SETTINGS_URL_MAX    128
#define SETTINGS_TOKEN_LEN  64

typedef struct {
    char     ssid[SETTINGS_SSID_MAX + 1];
    char     pass[SETTINGS_PASS_MAX + 1];
    char     server[SETTINGS_URL_MAX + 1];
    char     token[SETTINGS_TOKEN_LEN + 1];
    uint32_t min_checkin_s;
} settings_t;

/* Open NVS (initialising it if needed) and load, falling back to Kconfig. */
void settings_load(settings_t *s);

/* Each validates, stores and updates `s`; false (and nothing stored) if the
 * value is not acceptable. */
bool settings_set_wifi(settings_t *s, const char *ssid, const char *pass);
bool settings_set_server(settings_t *s, const char *url);
bool settings_set_token(settings_t *s, const char *token);
bool settings_set_min_checkin(settings_t *s, uint32_t seconds);

/* True when there is enough to try the website at all. */
bool settings_remote_ready(const settings_t *s);

/* A server URL the device will use: https:// and nothing that needs quoting. */
bool settings_url_ok(const char *url);

#endif /* SETTINGS_H */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "remote"

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap,
                     const char *fallback)
{
    size_t len = cap;
    if (h == 0 || nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strncpy(dst, fallback, cap - 1);
        dst[cap - 1] = '\0';
    }
}

static bool token_ok(const char *token)
{
    if (strlen(token) != SETTINGS_TOKEN_LEN) {
        return false;
    }
    for (const char *p = token; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool store(const char *key, const char *str, const uint32_t *u32)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = str ? nvs_set_str(h, key, str) : nvs_set_u32(h, key, *u32);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void settings_load(settings_t *s)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition from another firmware: start it over rather than run
         * without settings. */
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); using build defaults",
                 esp_err_to_name(err));
    }

    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        h = 0;
    }
    memset(s, 0, sizeof *s);
    load_str(h, "ssid", s->ssid, sizeof s->ssid, CONFIG_BMS_WIFI_SSID);
    load_str(h, "pass", s->pass, sizeof s->pass, CONFIG_BMS_WIFI_PASSWORD);
    load_str(h, "server", s->server, sizeof s->server, CONFIG_BMS_SERVER_URL);
    load_str(h, "token", s->token, sizeof s->token, "");
    if (h == 0 || nvs_get_u32(h, "min_checkin", &s->min_checkin_s) != ESP_OK) {
        s->min_checkin_s = 1;
    }
    if (h == 0 || nvs_get_u32(h, "live", &s->live_s) != ESP_OK || s->live_s > 3600u) {
        s->live_s = CONFIG_BMS_LIVE_S;
    }
    if (h) {
        nvs_close(h);
    }
    if (s->token[0] && !token_ok(s->token)) {
        ESP_LOGW(TAG, "stored token is malformed; ignoring it");
        s->token[0] = '\0';
    }
    if (!settings_url_ok(s->server)) {
        ESP_LOGW(TAG, "stored server URL rejected; using the build default");
        strncpy(s->server, CONFIG_BMS_SERVER_URL, sizeof s->server - 1);
    }
}

bool settings_url_ok(const char *url)
{
    if (strncmp(url, "https://", 8) != 0 || strlen(url) > SETTINGS_URL_MAX) {
        return false;
    }
    for (const char *p = url; *p; p++) {
        if (*p <= ' ' || *p > '~' || *p == '"' || *p == '\\' || *p == '?' ||
            *p == '#') {
            return false;
        }
    }
    return true;
}

bool settings_set_wifi(settings_t *s, const char *ssid, const char *pass)
{
    const size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl == 0 || sl > SETTINGS_SSID_MAX || pl > SETTINGS_PASS_MAX ||
        (pl > 0 && pl < 8)) {
        return false;
    }
    /* 64 characters is a raw WPA2 key, which must be hex. */
    for (size_t i = 0; pl == SETTINGS_PASS_MAX && i < pl; i++) {
        const char c = pass[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    if (!store("ssid", ssid, NULL) || !store("pass", pass, NULL)) {
        return false;
    }
    strcpy(s->ssid, ssid);
    strcpy(s->pass, pass);
    return true;
}

bool settings_set_server(settings_t *s, const char *url)
{
    if (!settings_url_ok(url) || !store("server", url, NULL)) {
        return false;
    }
    strcpy(s->server, url);
    return true;
}

bool settings_set_token(settings_t *s, const char *token)
{
    if (!token_ok(token) || !store("token", token, NULL)) {
        return false;
    }
    strcpy(s->token, token);
    return true;
}

bool settings_set_min_checkin(settings_t *s, uint32_t seconds)
{
    if (seconds < 1 || seconds > 1209600u || !store("min_checkin", NULL, &seconds)) {
        return false;
    }
    s->min_checkin_s = seconds;
    return true;
}

bool settings_set_live(settings_t *s, uint32_t seconds)
{
    if (seconds > 3600u || !store("live", NULL, &seconds)) {
        return false;
    }
    s->live_s = seconds;
    return true;
}

bool settings_remote_ready(const settings_t *s)
{
    return s->ssid[0] != '\0' && token_ok(s->token) &&
           settings_url_ok(s->server);
}

#include "net.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";

#define BIT_UP   BIT0
#define BIT_FAIL BIT1

/* esp_http_client's timeout is per socket operation, so a server trickling one
 * byte at a time could hold the radio on for hours. Each operation gets a
 * short timeout, and the whole request a hard deadline on top. */
#define HTTP_OP_TIMEOUT_MS 5000
#define HTTP_DEADLINE_US   (20 * 1000 * 1000)

static const settings_t *g_settings;
static EventGroupHandle_t g_events;
static bool g_started;

/* The last good access point, to skip the scan after a deep-sleep wake. */
RTC_DATA_ATTR static uint8_t g_ap_channel;
RTC_DATA_ATTR static uint8_t g_ap_bssid[6];

static uint8_t g_last_reason;

/* The access point's reason, in words, for the few that point at a fix. */
static const char *reason_text(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found (name, range, or 5 GHz only?)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "refused: wrong password?";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network found, but its security type is not accepted";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "signal too weak";
    case WIFI_REASON_ASSOC_LEAVE:
        return "disconnected by us";
    default:
        return "see esp_wifi_types.h";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_last_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupClearBits(g_events, BIT_UP);
        xEventGroupSetBits(g_events, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            g_ap_channel = ap.primary;
            memcpy(g_ap_bssid, ap.bssid, sizeof g_ap_bssid);
        }
        xEventGroupSetBits(g_events, BIT_UP);
    }
}

void net_init(const settings_t *s)
{
    g_settings = s;
    g_events = xEventGroupCreate();

    /* The HTTP client logs request headers at debug level; keep it and TLS
     * to warnings so the bearer token can never reach the serial log. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* A client of the club's access point; without this, set_config and
     * connect fail with ESP_ERR_WIFI_MODE. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_event, NULL));
}

static bool try_connect(bool fast, uint32_t timeout_ms)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, g_settings->ssid, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, g_settings->pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = g_settings->pass[0] ? WIFI_AUTH_WPA2_PSK
                                                    : WIFI_AUTH_OPEN;
    if (fast) {
        wc.sta.channel = g_ap_channel;
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, g_ap_bssid, sizeof wc.sta.bssid);
    }

    xEventGroupClearBits(g_events, BIT_UP | BIT_FAIL);
    const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config: %s", esp_err_to_name(cfg_err));
        return false;
    }
    if (!g_started) {
        if (esp_wifi_start() != ESP_OK) {
            return false;
        }
        g_started = true;
        /* Between beacons the radio sleeps; the link stays associated. */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    /* A first association is often refused once (the access point still
     * clearing an old one), so retry until the time is up. */
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        g_last_reason = 0;
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
        const int64_t left_us = deadline - esp_timer_get_time();
        const EventBits_t bits = xEventGroupWaitBits(
            g_events, BIT_UP | BIT_FAIL, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(left_us > 0 ? left_us / 1000 : 0));
        if (bits & BIT_UP) {
            return true;
        }
        if (!(bits & BIT_FAIL)) {
            break;   /* timed out */
        }
        ESP_LOGW(TAG, "Wi-Fi \"%s\": reason %u, %s", g_settings->ssid,
                 (unsigned)g_last_reason, reason_text(g_last_reason));
        xEventGroupClearBits(g_events, BIT_FAIL);
        if (esp_timer_get_time() + 500 * 1000 >= deadline) {
            return false;   /* the last disconnect has landed already */
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Let this attempt's disconnect event land now, so it cannot end the
     * next attempt early. */
    xEventGroupClearBits(g_events, BIT_FAIL);
    esp_wifi_disconnect();
    xEventGroupWaitBits(g_events, BIT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    return false;
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise";
    default:                        return "other";
    }
}

/* Once per boot, after a failed connect: what the radio can actually see, so
 * a wrong name, a 5 GHz-only network or a weak signal shows in the log. */
static void log_visible_networks(void)
{
    static bool done;
    if (done || !g_started) {
        return;
    }
    done = true;

    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    uint16_t n = 12;
    wifi_ap_record_t aps[12];
    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "%u networks visible (2.4 GHz only):", (unsigned)n);
    for (uint16_t i = 0; i < n; i++) {
        ESP_LOGW(TAG, "  \"%s\"  ch %u  %d dBm  %s%s", (const char *)aps[i].ssid,
                 (unsigned)aps[i].primary, aps[i].rssi, auth_name(aps[i].authmode),
                 strcmp((const char *)aps[i].ssid, g_settings->ssid) == 0 ? "  <- configured" : "");
    }
}

static volatile bool g_tried;

bool net_tried(void)
{
    return g_tried;
}

bool net_rssi(int *dbm)
{
    wifi_ap_record_t ap;
    if (!net_is_up() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *dbm = ap.rssi;
    return true;
}

bool net_up(uint32_t timeout_ms)
{
    g_tried = true;
    if (net_is_up()) {
        return true;
    }
    if (g_settings->ssid[0] == '\0') {
        return false;
    }
    if (g_ap_channel != 0 && try_connect(true, timeout_ms / 2)) {
        return true;
    }
    /* The remembered access point is gone or moved: scan properly. */
    g_ap_channel = 0;
    if (try_connect(false, timeout_ms)) {
        return true;
    }
    ESP_LOGW(TAG, "Wi-Fi \"%s\" not reachable", g_settings->ssid);
    log_visible_networks();
    return false;
}

void net_down(void)
{
    if (g_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        g_started = false;
    }
    xEventGroupClearBits(g_events, BIT_UP);
}

bool net_is_up(void)
{
    return g_events && (xEventGroupGetBits(g_events) & BIT_UP);
}

static int request(void *ctx, const char *method, const char *path,
                   const char *body, size_t body_len,
                   char *resp, size_t resp_cap, size_t *resp_len)
{
    (void)ctx;
    *resp_len = 0;

    char url[SETTINGS_URL_MAX + 256];
    char auth[16 + SETTINGS_TOKEN_LEN];
    if ((size_t)snprintf(url, sizeof url, "%s%s", g_settings->server, path) >= sizeof url) {
        return REMOTE_ERR_TRANSPORT;
    }
    snprintf(auth, sizeof auth, "Bearer %s", g_settings->token);

    const bool post = strcmp(method, "POST") == 0;
    const esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms            = HTTP_OP_TIMEOUT_MS,
        .crt_bundle_attach     = esp_crt_bundle_attach,   /* verify the server */
        .disable_auto_redirect = true,                    /* a 3xx is an error */
        .keep_alive_enable     = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return REMOTE_ERR_TRANSPORT;
    }
    esp_http_client_set_header(h, "Authorization", auth);
    if (post) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }

    const int64_t deadline = esp_timer_get_time() + HTTP_DEADLINE_US;
    int status = REMOTE_ERR_TRANSPORT;
    if (esp_http_client_open(h, post ? (int)body_len : 0) == ESP_OK &&
        (!post || esp_http_client_write(h, body, (int)body_len) == (int)body_len) &&
        esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        /* Read at most resp_cap bytes, then one more to tell "exactly full"
         * from "longer than we accept". */
        while (*resp_len < resp_cap) {
            if (esp_timer_get_time() > deadline) {
                status = REMOTE_ERR_TRANSPORT;
                break;
            }
            const int n = esp_http_client_read(h, resp + *resp_len,
                                               (int)(resp_cap - *resp_len));
            if (n <= 0) {
                break;
            }
            *resp_len += (size_t)n;
        }
        char extra;
        if (status != REMOTE_ERR_TRANSPORT && *resp_len == resp_cap &&
            esp_http_client_read(h, &extra, 1) > 0) {
            status = REMOTE_ERR_TOO_LONG;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    memset(auth, 0, sizeof auth);
    return status;
}

void net_transport(remote_transport_t *t)
{
    t->request = request;
    t->ctx     = NULL;
}

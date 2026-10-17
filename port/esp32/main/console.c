#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "linenoise/linenoise.h"

static const char *TAG = "console";

static settings_t *g_s;
static int64_t g_opened_us;
static volatile bool g_checkin;
static bool g_network;

bool console_is_open(void)
{
    return g_s != NULL &&
           esp_timer_get_time() - g_opened_us < (int64_t)CONSOLE_WINDOW_S * 1000000;
}

void console_set_network(bool running)
{
    g_network = running;
}

bool console_take_checkin_request(void)
{
    const bool r = g_checkin;
    g_checkin = false;
    return r;
}

static bool closed(void)
{
    if (!console_is_open()) {
        printf("console closed - reset, then touch a pad within 10 s to reopen it\n");
        return true;
    }
    return false;
}

static int cmd_wifi(int argc, char **argv)
{
    /* The line with the password is already in linenoise's history (RAM
     * only); drop it whatever happens next. */
    linenoiseHistoryFree();
    if (closed()) return 1;
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [password]\n");
        return 1;
    }
    const bool ok = settings_set_wifi(g_s, argv[1], argc == 3 ? argv[2] : "");
    printf(ok ? "wifi saved - reset the board to use it\n"
              : "rejected (ssid 1-32 chars, password empty or 8-63, or 64 hex)\n");
    return ok ? 0 : 1;
}

static int cmd_server(int argc, char **argv)
{
    if (closed()) return 1;
    if (argc != 2 || !settings_set_server(g_s, argv[1])) {
        printf("usage: server https://host/wp-json/avia-bms/v1\n");
        return 1;
    }
    printf("server saved - reset the board to use it\n");
    return 0;
}

static int cmd_token(int argc, char **argv)
{
    linenoiseHistoryFree();
    if (closed()) return 1;
    if (argc != 2 || !settings_set_token(g_s, argv[1])) {
        printf("usage: token <64 lowercase hex characters, from WP admin>\n");
        return 1;
    }
    printf("token saved - reset the board to use it\n");
    return 0;
}

static int cmd_min_checkin(int argc, char **argv)
{
    if (closed()) return 1;
    char *end = NULL;
    const unsigned long v = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || *end != '\0' || !settings_set_min_checkin(g_s, (uint32_t)v)) {
        printf("usage: min_checkin <seconds, 1-1209600>\n");
        return 1;
    }
    printf("min_checkin %lu s (takes effect after the next reset)\n", v);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Secrets are shown as set or not, never their value. */
    printf("wifi        %s (password %s)\n", g_s->ssid[0] ? g_s->ssid : "-",
           g_s->pass[0] ? "set" : "none");
    printf("server      %s\n", g_s->server);
    printf("token       %s\n", g_s->token[0] ? "set" : "NOT SET");
    printf("min_checkin %lu s\n", (unsigned long)g_s->min_checkin_s);
    printf("console     %s\n", console_is_open() ? "open" : "closed");
    printf("network     %s\n", g_network ? "running" : "not started (reset after setting wifi and token)");
    return 0;
}

static int cmd_checkin(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (closed()) return 1;
    if (!g_network) {
        printf("no network yet: set wifi and token, then reset the board\n");
        return 1;
    }
    g_checkin = true;
    printf("checking in - watch the log\n");
    return 0;
}

void console_start(settings_t *s)
{
    g_s = s;
    g_opened_us = esp_timer_get_time();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "bms> ";
    rc.max_history_len = 1;   /* linenoise refuses 0; secrets are dropped from it */
    esp_console_dev_usb_serial_jtag_config_t dev =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&dev, &rc, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "console did not start");
        g_s = NULL;
        return;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "wifi <ssid> [password]", .func = cmd_wifi },
        { .command = "server", .help = "server <https URL of the API>", .func = cmd_server },
        { .command = "token", .help = "token <device token from WP admin>", .func = cmd_token },
        { .command = "min_checkin", .help = "min_checkin <s>: refuse shorter intervals",
          .func = cmd_min_checkin },
        { .command = "status", .help = "show settings (secrets as set/not set)",
          .func = cmd_status },
        { .command = "checkin", .help = "check in with the website now", .func = cmd_checkin },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
    ESP_LOGW(TAG, "console open for %d s - type help", CONSOLE_WINDOW_S);
}

/*
 * USB serial console for the settings in settings.h.
 *
 * Open only when a pad is touched within the first seconds after a power-on
 * or reset, and only for
 * CONSOLE_WINDOW_S after that: the USB-C port is also where the power bank
 * plugs in, so it is reachable by anyone at the bank. While it is open the
 * device does not deep-sleep (that would cut the USB link mid-command).
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

#define CONSOLE_WINDOW_S 300

/* Start the console. `s` is updated in place by the commands. */
void console_start(settings_t *s);

/* True while commands are accepted. */
bool console_is_open(void);

/* Whether the network task runs (Wi-Fi and token were set at boot). */
void console_set_network(bool running);

/* Set by the "checkin" command; read and cleared by the main loop. */
bool console_take_checkin_request(void);

#endif /* CONSOLE_H */

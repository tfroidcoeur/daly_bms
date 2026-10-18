/*
 * Wi-Fi and HTTPS for core/remote.c.
 *
 * Wi-Fi is brought up for a check-in and taken down before deep sleep. The
 * channel and access point of the last good connection are kept in RTC memory,
 * so a wake from deep sleep reconnects without a full scan - the scan is most
 * of what a check-in costs.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

#include "remote.h"
#include "settings.h"

/* Once, at boot. `s` must outlive the program (it is read on every request). */
void net_init(const settings_t *s);

/* Connect if not connected. False on timeout or bad credentials. */
bool net_up(uint32_t timeout_ms);

/* Disconnect and switch the radio off. */
void net_down(void);

bool net_is_up(void);

/* The access point's signal in dBm; false when not connected. */
bool net_rssi(int *dbm);

/* True once a connect has been tried since boot (for the screen's icon). */
bool net_tried(void);

/* Fill `t` with the esp_http_client transport. */
void net_transport(remote_transport_t *t);

#endif /* NET_H */

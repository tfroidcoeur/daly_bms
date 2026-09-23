/*
 * CAN link on the ESP32-S3 TWAI controller.
 *
 * Same surface as the host SocketCAN link, so core/ sees no difference.
 */
#ifndef TWAI_LINK_H
#define TWAI_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* 250 kbit/s, extended frames, accepting only BMS responses to host 0x40. */
bool twai_link_start(void);
void twai_link_stop(void);

bool twai_link_send(uint32_t ext_id, const uint8_t *data, uint8_t len);

/* Non-blocking. Returns false when nothing is queued. */
bool twai_link_recv(uint32_t *ext_id, uint8_t *data, uint8_t *len);

/*
 * Bus health. Every output is written, including when the driver is not
 * installed - the caller passes uninitialised locals and branches on them.
 *
 * `needs_recovery` covers both halves of a bus fault: the controller going
 * bus-off, and the stopped state that recovery leaves behind.
 */
void twai_link_stats(uint32_t *tx_failed, uint32_t *rx_missed,
                     uint32_t *bus_errors, bool *needs_recovery);

/*
 * Drive one step of bus-off recovery. Call whenever twai_link_stats() reports
 * needs_recovery; it takes two calls to get from bus-off back to running, so
 * this is written to be called repeatedly and do nothing when it should.
 */
void twai_link_recover(void);

#endif /* TWAI_LINK_H */

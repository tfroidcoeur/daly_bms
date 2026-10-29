/*
 * CAN link on the ESP32-S3 TWAI controller.
 *
 * Same surface as the host SocketCAN link, so core/ sees no difference.
 */
#ifndef TWAI_LINK_H
#define TWAI_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "poller.h"
#include "sdkconfig.h"

/*
 * 250 kbit/s. Receives only Daly BMS responses to host 0x40 (extended frames);
 * sends either kind, so the virtual BMS can broadcast its 11-bit frames - here,
 * or on a second bus with twai_link_send_victron().
 */
bool twai_link_start(void);
void twai_link_stop(void);

/* `ext`: 29-bit identifier; otherwise 11-bit. */
bool twai_link_send(uint32_t id, bool ext, const uint8_t *data, uint8_t len);

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

/*
 * Send the virtual BMS's `n` frames. Without CONFIG_BMS_VICTRON_SEPARATE_BUS
 * that is just twai_link_send() on the pack bus.
 *
 * With it, they go out on the second transceiver, and then it comes
 * back to the packs. The one controller is reinstalled on the other pins and
 * bit rate and back again - a few milliseconds, longest (about 5 ms) when
 * nobody on that bus acknowledges. Call it in the gap before a Daly request,
 * so no reply is in flight; anything still in the receive queue is lost.
 *
 * The reinstall clears the controller's error state, so while this is in use a
 * pack bus that went bus-off is re-armed every second instead of recovering
 * through twai_link_recover(). twai_link_stats() describes the pack bus only.
 *
 * Returns false if a frame was not acknowledged.
 */
bool twai_link_send_victron(const can_frame_out_t *f, uint8_t n);

#endif /* TWAI_LINK_H */

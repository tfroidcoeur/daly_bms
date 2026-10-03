/*
 * SocketCAN link for the host simulator.
 *
 * Mirrors the small surface the ESP32 TWAI port provides, so core/ and ui/ see
 * the same shape on both platforms.
 */
#ifndef CAN_SOCKETCAN_H
#define CAN_SOCKETCAN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int fd;
} can_link_t;

/*
 * Open `ifname` (e.g. "vcan0") non-blocking. Returns false and prints why.
 *
 * `filtered` installs the kernel filter that accepts only BMS responses to host
 * 0x40. Pass false for diagnostics: a pack answering with an identifier we did
 * not predict is invisible through the filter, and invisible looks exactly like
 * a pack that is not answering at all.
 *
 * A non-zero `virtual_addr` adds requests to that address to what the filter
 * lets in, for the virtual BMS.
 */
bool can_link_open(can_link_t *l, const char *ifname, bool filtered,
                   uint8_t virtual_addr);
void can_link_close(can_link_t *l);

bool can_link_send(can_link_t *l, uint32_t ext_id, const uint8_t *data, uint8_t len);

/*
 * Non-blocking receive of one extended frame. Returns false when nothing is
 * waiting. Standard-ID frames are skipped rather than reported as an empty
 * queue, so a stray one cannot cut a caller's drain loop short.
 */
bool can_link_recv(can_link_t *l, uint32_t *ext_id, uint8_t *data, uint8_t *len);

/*
 * Receive one frame of any kind, for the raw logger. `ext` says whether the
 * identifier was extended; `id` is masked accordingly.
 */
bool can_link_recv_any(can_link_t *l, uint32_t *id, uint8_t *data, uint8_t *len,
                       bool *ext);

/* Milliseconds since the first call. Monotonic. */
uint32_t host_millis(void);

#endif /* CAN_SOCKETCAN_H */

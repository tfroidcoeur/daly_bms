#include "can_socketcan.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool can_link_open(can_link_t *l, const char *ifname, bool filtered)
{
    l->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (l->fd < 0) {
        fprintf(stderr, "socket(PF_CAN): %s\n", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(l->fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr,
                "no such CAN interface '%s': %s\n"
                "  ./tools/setup-vcan.sh %s\n",
                ifname, strerror(errno), ifname);
        close(l->fd);
        l->fd = -1;
        return false;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(l->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "bind(%s): %s\n", ifname, strerror(errno));
        close(l->fd);
        l->fd = -1;
        return false;
    }

    /*
     * Only accept BMS responses: priority 0x18 (bits 24-28) and destination
     * 0x40 (bits 8-15). The command byte (bits 16-23) and the source address
     * (bits 0-7) must stay unmasked - they are exactly what varies.
     *
     * Both can_id and can_mask carry CAN_EFF_FLAG, because the kernel presents
     * received extended identifiers with that bit set. Setting it in only one
     * of the two matches nothing.
     */
    if (filtered) {
        struct can_filter filt = {
            .can_id   = (0x18u << 24) | (0x40u << 8) | CAN_EFF_FLAG,
            .can_mask = 0x1F00FF00u | CAN_EFF_FLAG,
        };
        if (setsockopt(l->fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filt,
                       sizeof filt) < 0) {
            fprintf(stderr, "setsockopt(CAN_RAW_FILTER): %s\n", strerror(errno));
        }
    }
    /* Unfiltered is the socket default: every frame on the wire. */

    const int flags = fcntl(l->fd, F_GETFL, 0);
    fcntl(l->fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void can_link_close(can_link_t *l)
{
    if (l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
    }
}

bool can_link_send(can_link_t *l, uint32_t ext_id, const uint8_t *data, uint8_t len)
{
    struct can_frame f;
    memset(&f, 0, sizeof f);
    f.can_id  = (ext_id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    f.can_dlc = len;
    memcpy(f.data, data, len > 8 ? 8 : len);

    const ssize_t n = write(l->fd, &f, sizeof f);
    return n == (ssize_t)sizeof f;
}

bool can_link_recv_any(can_link_t *l, uint32_t *id, uint8_t *data, uint8_t *len,
                       bool *ext)
{
    struct can_frame f;
    memset(&f, 0, sizeof f);
    const ssize_t n = read(l->fd, &f, sizeof f);
    if (n != (ssize_t)sizeof f) {
        return false;   /* EAGAIN on an empty queue */
    }
    *ext  = (f.can_id & CAN_EFF_FLAG) != 0;
    *id   = f.can_id & (*ext ? CAN_EFF_MASK : CAN_SFF_MASK);
    *len  = f.can_dlc > 8 ? 8 : f.can_dlc;
    memcpy(data, f.data, 8);
    return true;
}

bool can_link_recv(can_link_t *l, uint32_t *ext_id, uint8_t *data, uint8_t *len)
{
    /*
     * Callers drain with `while (can_link_recv(...))`, so a frame we do not want
     * has to be skipped rather than reported as "queue empty" - otherwise one
     * stray standard-ID frame ends the pass and strands the backlog.
     */
    for (;;) {
        bool ext;
        if (!can_link_recv_any(l, ext_id, data, len, &ext)) {
            return false;
        }
        if (ext) {
            return true;
        }
    }
}

uint32_t host_millis(void)
{
    static struct timespec t0;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (t0.tv_sec == 0 && t0.tv_nsec == 0) {
        t0 = now;
    }
    return (uint32_t)((now.tv_sec - t0.tv_sec) * 1000 +
                      (now.tv_nsec - t0.tv_nsec) / 1000000);
}

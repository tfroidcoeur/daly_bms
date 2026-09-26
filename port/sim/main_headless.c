/*
 * Headless host monitor: core/ driven against a real SocketCAN interface.
 *
 * Same poller and same decoder as the device firmware, printed as text instead
 * of drawn on a screen. This is the bring-up tool: point it at a USB-CAN
 * adapter on the real packs and read what the decoder makes of them.
 *
 *     ./build/sim_headless can0            decoded model, refreshed twice a second
 *     ./build/sim_headless can0 --raw      every frame, with its decode alongside
 *
 * --raw is the counterpart of CONFIG_BMS_RAW_LOGGER in the firmware, and it
 * drops the kernel filter as well: a pack answering with an identifier we did
 * not predict shows up here, where in the normal view it is indistinguishable
 * from a pack that never answered. Run it first, against one pack, and check
 * every field against Daly's own app before trusting the decoded view.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bms_model.h"
#include "can_socketcan.h"
#include "daly_proto.h"
#include "poller.h"

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/*
 * Transmit failures are the quietest way for a bench setup to be broken: with
 * nothing on the bus to ACK - one terminator, wrong bit rate, CANH/CANL swapped
 * - every write fails and the screen just stays empty. Count them and say so.
 */
static uint32_t g_tx_ok, g_tx_fail, g_rx_frames, g_rx_undecoded;

static void send_frame(can_link_t *link, const can_frame_out_t *tx)
{
    if (can_link_send(link, tx->id, tx->data, tx->len)) {
        g_tx_ok++;
    } else {
        g_tx_fail++;
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void print_model(const system_model_t *m)
{
    const bms_summary_t s = bms_model_summary(m);

    printf("\033[H\033[2J");   /* home + clear */
    printf("Daly BMS monitor - headless core check\n");
    printf("=====================================\n\n");
    printf("bank   %2u/%u online   %6.2f V   %+7.2f A   %5.1f %%   "
           "worst spread %u mV%s\n\n",
           s.online_count, BMS_PACK_COUNT, s.bank_mv / 1000.0,
           s.total_ma / 1000.0, s.mean_soc_pct_x10 / 10.0,
           s.worst_cell_delta_mv, s.any_alarm ? "   ** ALARM **" : "");

    for (int i = 0; i < BMS_PACK_COUNT; i++) {
        const bms_pack_t *p = &m->pack[i];
        if (!p->online) {
            printf("pack %u (0x%02X)   NO DATA\n\n", i + 1, p->addr);
            continue;
        }
        /* Cycles come from bytes Daly's own document reserves, so say nothing
         * about them unless this pack actually fills them. */
        char cycles[24] = "cycles n/r";
        if (p->cycles_valid) {
            snprintf(cycles, sizeof cycles, "%u cycles", p->cycles);
        }
        printf("pack %u (0x%02X)   %6.2f V  %+7.2f A  %6.1f W  SoC %5.1f %%  "
               "%s  %s%s\n",
               i + 1, p->addr, p->pack_mv / 1000.0, p->pack_ma / 1000.0,
               (double)bms_pack_watts(p), p->soc_pct_x10 / 10.0, cycles,
               p->chg_mos ? "CHG " : "chg ", p->dsg_mos ? "DSG" : "dsg");
        printf("              cells %u  min %u mV (#%u)  max %u mV (#%u)  "
               "spread %u mV   temp %d..%d C\n",
               p->cell_count, p->cell_min_mv, p->cell_min_idx,
               p->cell_max_mv, p->cell_max_idx, bms_pack_cell_delta_mv(p),
               p->temp_min_c, p->temp_max_c);

        if (p->cells_valid) {
            printf("              ");
            for (uint8_t c = 0; c < p->cell_count; c++) {
                printf("%u%s", p->cell_mv[c],
                       (c + 1) % 8 == 0 && c + 1 < p->cell_count
                           ? "\n              " : "  ");
            }
            printf("\n");
        }
        if (p->alarm_active) {
            printf("              ALARM %02X %02X %02X %02X %02X %02X %02X\n",
                   p->alarms[0], p->alarms[1], p->alarms[2], p->alarms[3],
                   p->alarms[4], p->alarms[5], p->alarms[6]);
        }
        printf("              frames %u\n\n", p->frames_rx);
    }
    printf("link   tx %u ok", g_tx_ok);
    if (g_tx_fail) {
        printf(", %u FAILED - nothing is ACKing: check termination, bit rate, "
               "CANH/CANL", g_tx_fail);
    }
    printf("   rx %u\n", g_rx_frames);

    printf("\nCtrl-C to stop.\n");
    fflush(stdout);
}

/* ---- raw mode ------------------------------------------------------------ */

static const char *cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case DALY_CMD_SOC:         return "0x90 soc";
    case DALY_CMD_CELL_MINMAX: return "0x91 cell min/max";
    case DALY_CMD_TEMP_MINMAX: return "0x92 temp min/max";
    case DALY_CMD_MOS:         return "0x93 mos";
    case DALY_CMD_STATUS:      return "0x94 status";
    case DALY_CMD_CELL_VOLTS:  return "0x95 cell volts";
    case DALY_CMD_CELL_TEMPS:  return "0x96 cell temps";
    case DALY_CMD_BALANCE:     return "0x97 balance";
    case DALY_CMD_FAULTS:      return "0x98 faults";
    default:                   return NULL;
    }
}

/* What the decoder made of the bytes, so the two can be compared in one line. */
static void annotate(uint8_t cmd, const uint8_t *d, char *buf, size_t n)
{
    switch (cmd) {
    case DALY_CMD_SOC:
        snprintf(buf, n, "%.2f V  %+.1f A  %.1f %%",
                 (int32_t)((d[0] << 8) | d[1]) * 100 / 1000.0,
                 daly_decode_current_ma((uint16_t)((d[4] << 8) | d[5])) / 1000.0,
                 ((d[6] << 8) | d[7]) / 10.0);
        break;
    case DALY_CMD_CELL_MINMAX:
        snprintf(buf, n, "max %u mV #%u  min %u mV #%u",
                 (d[0] << 8) | d[1], d[2], (d[3] << 8) | d[4], d[5]);
        break;
    case DALY_CMD_TEMP_MINMAX:
        snprintf(buf, n, "max %d C #%u  min %d C #%u",
                 daly_decode_temp_c(d[0]), d[1], daly_decode_temp_c(d[2]), d[3]);
        break;
    case DALY_CMD_STATUS:
        /* Bytes 5-7: reserved per Daly's document, cycle count per the layout
         * this was built from. Printed raw so a real pack can settle it. */
        snprintf(buf, n, "%u cells  %u sensors  b5-7 %02X %02X %02X",
                 d[0], d[1], d[5], d[6], d[7]);
        break;
    case DALY_CMD_CELL_VOLTS:
        /* Byte 0 is the frame number - from 0 per Daly's document, from 1 per
         * most firmware. Whether the first burst starts at 0 is the answer. */
        snprintf(buf, n, "frame %u: %u %u %u mV", d[0],
                 (d[1] << 8) | d[2], (d[3] << 8) | d[4], (d[5] << 8) | d[6]);
        break;
    case DALY_CMD_MOS:
        snprintf(buf, n, "state %u  chg %s  dsg %s  %lu mAh  life %u",
                 d[0], d[1] ? "ON" : "off", d[2] ? "ON" : "off",
                 (unsigned long)(((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                                 ((uint32_t)d[6] << 8) | d[7]),
                 d[3]);
        break;
    case DALY_CMD_BALANCE:
    case DALY_CMD_FAULTS:
        /* Bit meanings are not confirmed against hardware; show them as bits
         * rather than inventing names for them. */
        snprintf(buf, n, "bits (meanings unconfirmed)");
        break;
    case DALY_CMD_CELL_TEMPS:
        snprintf(buf, n, "frame %u: %d %d %d %d %d %d %d C", d[0],
                 daly_decode_temp_c(d[1]), daly_decode_temp_c(d[2]),
                 daly_decode_temp_c(d[3]), daly_decode_temp_c(d[4]),
                 daly_decode_temp_c(d[5]), daly_decode_temp_c(d[6]),
                 daly_decode_temp_c(d[7]));
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void raw_loop(can_link_t *link)
{
    system_model_t model;
    poller_t poller;
    bms_model_init(&model);
    poller_init(&poller, &model, poller_default_cfg());

    printf("raw frame log - unfiltered, every frame on the bus.\n"
           "Check each decode against Daly's own app before trusting the UI.\n"
           "Ctrl-C to stop.\n\n");

    uint32_t last_summary = 0;

    while (!g_stop) {
        const uint32_t now = host_millis();

        can_frame_out_t tx;
        if (poller_tick(&poller, now, &tx)) {
            send_frame(link, &tx);
            const char *name = cmd_name((uint8_t)((tx.id >> 16) & 0xFF));
            printf("%8u  TX %08X  ->  %-18s pack 0x%02X%s\n",
                   now, tx.id, name ? name : "?",
                   (unsigned)((tx.id >> 8) & 0xFF),
                   g_tx_fail ? "   [TX FAILED]" : "");
        }

        uint32_t id;
        uint8_t data[8], len;
        bool ext;
        while (can_link_recv_any(link, &id, data, &len, &ext)) {
            g_rx_frames++;

            printf("%8u  RX %s%0*X  [%u]", now, ext ? "" : "  ",
                   ext ? 8 : 3, id, len);
            for (uint8_t i = 0; i < len; i++) {
                printf(" %02X", data[i]);
            }

            uint8_t cmd, src;
            if (!ext) {
                printf("   <- standard ID, not Daly\n");
                g_rx_undecoded++;
            } else if (!daly_decode_id(id, &cmd, &src)) {
                printf("   <- not addressed to host 0x40\n");
                g_rx_undecoded++;
            } else if (bms_model_by_addr(&model, src) == NULL) {
                printf("   <- %s from unknown pack 0x%02X\n",
                       cmd_name(cmd) ? cmd_name(cmd) : "?", src);
                g_rx_undecoded++;
            } else {
                char note[96];
                annotate(cmd, data, note, sizeof note);
                printf("   pack %u  %-18s %s\n", src,
                       cmd_name(cmd) ? cmd_name(cmd) : "?", note);
                poller_on_frame(&poller, id, data, len, now);
            }
        }

        if (now - last_summary >= 5000) {
            last_summary = now;
            printf("---- tx %u ok / %u failed   rx %u (%u not ours)\n",
                   g_tx_ok, g_tx_fail, g_rx_frames, g_rx_undecoded);
        }
        fflush(stdout);
        sleep_ms(2);
    }
}

int main(int argc, char **argv)
{
    const char *ifname = "vcan0";
    bool raw = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            raw = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [interface] [--raw]\n", argv[0]);
            return 2;
        } else {
            ifname = argv[i];
        }
    }

    can_link_t link;
    /* Raw mode sees the whole bus; the normal view keeps the kernel filter. */
    if (!can_link_open(&link, ifname, !raw)) {
        return 1;
    }
    signal(SIGINT, on_sigint);

    if (raw) {
        raw_loop(&link);
        can_link_close(&link);
        printf("\nstopped.\n");
        return 0;
    }

    system_model_t model;
    poller_t poller;
    bms_model_init(&model);
    poller_init(&poller, &model, poller_default_cfg());

    uint32_t last_print = 0;

    while (!g_stop) {
        const uint32_t now = host_millis();

        can_frame_out_t tx;
        if (poller_tick(&poller, now, &tx)) {
            send_frame(&link, &tx);
        }

        uint32_t id;
        uint8_t data[8], len;
        while (can_link_recv(&link, &id, data, &len)) {
            g_rx_frames++;
            poller_on_frame(&poller, id, data, len, now);
        }

        if (now - last_print >= 500) {
            last_print = now;
            print_model(&model);
        }
        sleep_ms(2);
    }

    can_link_close(&link);
    printf("\nstopped.\n");
    return 0;
}

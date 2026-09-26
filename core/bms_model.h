/*
 * The data model: what we know about each pack, and about the bank as a whole.
 *
 * Plain C99 structs with no dependencies. Everything is fixed-point integer -
 * no floats - so the same numbers appear on the host simulator and the device.
 *
 * Units are named in every field: _mv millivolts, _ma milliamps, _c degrees
 * Celsius, _pct_x10 tenths of a percent.
 */
#ifndef BMS_MODEL_H
#define BMS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define BMS_PACK_COUNT      3    /* three parallel packs */
#define BMS_MAX_CELLS      48
#define BMS_MAX_TEMPS      16
#define BMS_ALARM_BYTES     7

/* Frames carry 3 cells (0x95) or 7 temperatures (0x96) each. */
#define DALY_CELLS_PER_FRAME 3
#define DALY_TEMPS_PER_FRAME 7

/* 64 frame slots, comfortably over the 16 a 48-cell 0x95 burst needs. */
#define DALY_SEEN_BYTES 8

/*
 * Where byte 0 of a multi-frame response starts counting. Daly's own protocol
 * document V1.0 says 0; the firmware this project has met says 1. Nothing in a
 * frame declares which, so the decoder works it out per pack and per command -
 * see daly_proto.c. UNKNOWN is deliberately the zero value, because packs are
 * zeroed in several places and "not yet known" is the only safe default.
 */
typedef enum {
    DALY_FRAME_BASE_UNKNOWN = 0,
    DALY_FRAME_BASE_ZERO,       /* first frame of a burst carries byte 0 == 0 */
    DALY_FRAME_BASE_ONE,        /* first frame of a burst carries byte 0 == 1 */
} daly_frame_base_t;

/*
 * Multi-frame reassembly. 0x95 and 0x96 are in flight at the same time and each
 * needs its OWN arrival bitmap: sharing one lets an abandoned burst leave marks
 * that complete the other command's set, publishing frames that never arrived.
 * For the same reason each learns its frame base separately.
 *
 * Bitmap bits are positions within the burst, always counted from 0, whatever
 * the pack puts on the wire.
 */
typedef struct {
    uint8_t  seen_cells[DALY_SEEN_BYTES];  /* 0x95 burst positions */
    uint8_t  seen_temps[DALY_SEEN_BYTES];  /* 0x96 burst positions */
    uint8_t  base_cells;                   /* daly_frame_base_t, for 0x95 */
    uint8_t  base_temps;                   /* daly_frame_base_t, for 0x96 */
    uint16_t cell_mv[BMS_MAX_CELLS];
    int8_t   temp_c[BMS_MAX_TEMPS];
} daly_rx_scratch_t;

typedef struct {
    uint8_t  addr;            /* Daly board address, 0x01..0x03 */

    /* Liveness */
    bool     online;
    uint32_t last_seen_ms;
    uint32_t frames_rx;

    /*
     * 0x90. A pack counts as online from its first reply to any command, so
     * these stay zero until 0x90 itself lands - and zero is a legal reading.
     * Anything judging them must check the flag first.
     */
    bool     soc_valid;
    int32_t  pack_mv;
    int32_t  pack_ma;         /* positive = charging, negative = discharging */
    uint16_t soc_pct_x10;

    /* 0x91 */
    uint16_t cell_min_mv, cell_max_mv;
    uint8_t  cell_min_idx, cell_max_idx;   /* 1-based, as the BMS reports them */

    /* 0x92. Same caveat as soc_valid: 0 C is a reading, not an absence. */
    bool     temp_minmax_valid;
    int8_t   temp_min_c, temp_max_c;
    uint8_t  temp_min_idx, temp_max_idx;

    /* 0x93 */
    uint8_t  charge_state;    /* 0 idle, 1 charging, 2 discharging */
    bool     chg_mos, dsg_mos;
    uint8_t  bms_life;        /* increments every frame; a liveness counter */
    uint32_t remaining_mah;

    /* 0x94 */
    uint8_t  cell_count;
    uint8_t  temp_count;
    bool     charger_present, load_present;

    /*
     * Cycle count, read from bytes 5-6 - which Daly's own protocol document
     * V1.0 lists as reserved. Firmware that leaves them empty therefore reports
     * no cycles at all rather than zero cycles, and the flag is how anything
     * displaying it tells those two apart.
     */
    bool     cycles_valid;
    uint16_t cycles;

    /* 0x95 / 0x96, committed only when a full set has arrived */
    uint16_t cell_mv[BMS_MAX_CELLS];
    int8_t   temp_c[BMS_MAX_TEMPS];
    bool     cells_valid, temps_valid;

    /* 0x97 */
    uint64_t balance_bits;    /* bit N set = cell N+1 balancing */

    /* 0x98 */
    uint8_t  alarms[BMS_ALARM_BYTES];
    bool     alarm_active;

    daly_rx_scratch_t rx;     /* multi-frame reassembly, not for display */
} bms_pack_t;

typedef struct {
    bms_pack_t pack[BMS_PACK_COUNT];
} system_model_t;

/* Aggregates recomputed for the overview page; never stored, always derived. */
typedef struct {
    uint8_t  online_count;
    int32_t  total_ma;        /* sum over online packs */
    int32_t  bank_mv;         /* mean over online packs; packs are in parallel */
    uint16_t mean_soc_pct_x10;
    uint16_t worst_cell_delta_mv;  /* largest max-min spread across all packs */
    bool     any_alarm;

    /*
     * What the bank is doing, as the BMS units themselves report it (0x93)
     * rather than inferred from the current sign - a pack can be sitting at a
     * few hundred milliamps and still call itself idle.
     * 0 idle, 1 charging, 2 discharging.
     */
    uint8_t  charge_state;
    bool     charger_present;      /* a charger is connected to some pack */
} bms_summary_t;

/* Initialise the model and assign addresses 0x01..0x03. */
void bms_model_init(system_model_t *m);

/* Look up a pack by Daly address; NULL if the address is not one of ours. */
bms_pack_t *bms_model_by_addr(system_model_t *m, uint8_t addr);

/*
 * Mark packs that have not been heard from within `timeout_ms` as offline.
 * Call once per tick from the poller.
 */
void bms_model_age(system_model_t *m, uint32_t now_ms, uint32_t timeout_ms);

/*
 * Recompute the bank aggregates from the online packs only. Cheap enough to
 * call every redraw; nothing is cached. Note `bank_mv` is their mean rather
 * than their sum - the packs are in parallel.
 */
bms_summary_t bms_model_summary(const system_model_t *m);

/* Cell spread for one pack; 0 if no cell data has arrived yet. */
uint16_t bms_pack_cell_delta_mv(const bms_pack_t *p);

/* Pack power in watts, derived from voltage and current. */
int32_t bms_pack_watts(const bms_pack_t *p);

/* Bank power in watts: positive charging, negative discharging. */
int32_t bms_summary_watts(const bms_summary_t *s);

#endif /* BMS_MODEL_H */

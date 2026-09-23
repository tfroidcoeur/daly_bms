/*
 * Turn the raw model into a short list of things a person should act on.
 *
 * This lives in core/, not the UI, for the same reason the decoder does: the
 * rules are the part worth testing, and they are testable only if they are
 * free of LVGL and of any screen.
 *
 * Ordering is by severity then pack, so the most important line is always
 * first - the overview shows only the top few.
 */
#ifndef WARNINGS_H
#define WARNINGS_H

#include <stddef.h>

#include "bms_model.h"

/* ---- thresholds ----------------------------------------------------------
 *
 * Defaults are for a 24S LiFePO4 bank (60.0 V empty, 87.6 V full). Adjust to
 * the actual chemistry and cell count; every one of these is a judgement call,
 * not a law.
 *
 * Note the per-cell limits below are independent of series count - a cell is a
 * cell - but the pack-level ones are not.
 */

/* The packs are wired in parallel, so their voltages should track closely.
 * A sustained gap means a bad joint, an open fuse, or a contactor not closed. */
#define WARN_PACK_MV_MISMATCH     500      /* 0.5 V */

/* A cell reading outside this is not a low cell, it is a broken measurement:
 * a missing sense lead, a shorted cell, a dead channel. */
#define WARN_CELL_MIN_SANE_MV    2000
#define WARN_CELL_MAX_SANE_MV    4000

/* In-pack cell spread. Some drift is normal; this much is a balance problem. */
#define WARN_CELL_SPREAD_MV       100
#define WARN_CELL_SPREAD_ALARM_MV 200

#define WARN_SOC_LOW_PCT_X10      200      /* 20.0 % */
#define WARN_SOC_CRIT_PCT_X10     100      /* 10.0 % */

#define WARN_TEMP_HIGH_C           45
#define WARN_TEMP_HIGH_ALARM_C     55
#define WARN_TEMP_LOW_C             2
#define WARN_TEMP_LOW_ALARM_C     (-5)

typedef enum {
    WARN_LEVEL_WARN = 0,   /* worth knowing */
    WARN_LEVEL_ALARM,      /* worth stopping for */
} warn_level_t;

typedef enum {
    WARN_PACK_OFFLINE,
    WARN_PACK_VOLTAGE_MISMATCH,
    WARN_CELL_COUNT_MISMATCH,
    WARN_CELL_IMPLAUSIBLE,
    WARN_CELL_SPREAD,
    WARN_BMS_FAULT,
    WARN_SOC_LOW,
    WARN_TEMP_HIGH,
    WARN_TEMP_LOW,
    WARN_CHARGING_BELOW_ZERO,
} warn_code_t;

typedef struct {
    warn_code_t  code;
    warn_level_t level;
    uint8_t      pack;   /* Daly address 0x01..0x03, or 0 for the whole bank */
    int32_t      a, b;   /* code-specific detail, see warnings_format() */
} warning_t;

#define WARN_MAX 12

typedef struct {
    uint8_t   count;
    bool      truncated;   /* more were found than would fit */
    warning_t item[WARN_MAX];
} warning_set_t;

/* Evaluate every rule against the model. Never allocates, never blocks. */
void warnings_evaluate(const system_model_t *m, warning_set_t *out);

/*
 * Render one warning as a single short line, for a display roughly 50
 * characters wide. Always NUL-terminates.
 */
void warnings_format(const warning_t *w, char *buf, size_t n);

/* True if any item is at alarm level. */
bool warnings_any_alarm(const warning_set_t *s);

#endif /* WARNINGS_H */

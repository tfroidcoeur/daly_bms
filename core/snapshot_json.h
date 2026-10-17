/*
 * The bank as one JSON snapshot, for the club website.
 *
 * The format is the contract in docs/remote-protocol.md: compact, keys in a
 * fixed order, integers only, null for "not reported yet". Golden outputs live
 * in docs/remote/fixtures/ and the tests compare against them byte for byte.
 *
 * Writes into the caller's buffer, never allocates, never reads past what the
 * model says is valid. Every string it emits is reduced to the protocol's
 * character set ([A-Za-z0-9 .,:%/()+-], at most 64 characters): anything else
 * becomes a space, so no output ever needs escaping.
 */
#ifndef SNAPSHOT_JSON_H
#define SNAPSHOT_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "bms_model.h"
#include "warnings.h"

#define SNAPSHOT_SCHEMA      1
#define SNAPSHOT_REQ_LEN     16      /* lowercase hex characters */
#define SNAPSHOT_TEXT_MAX    64

/* Comfortably over the worst case: three 48-cell packs, every fault bit set,
 * twelve warnings. */
#define SNAPSHOT_JSON_MAX    8192

typedef struct {
    const char *req;        /* the server's request id, SNAPSHOT_REQ_LEN hex */
    const char *fw;         /* "1.1.0" */
    uint32_t    uptime_s;
} snapshot_meta_t;

/*
 * Encode `m` and `w` into `buf`. Returns the length written, excluding the
 * NUL it always appends, or 0 if `cap` is too small (the buffer then holds an
 * empty string) or `meta->req` is not SNAPSHOT_REQ_LEN lowercase hex.
 */
size_t snapshot_json_encode(const system_model_t *m, const warning_set_t *w,
                            const snapshot_meta_t *meta, char *buf, size_t cap);

/*
 * Copy `src` into `dst` reduced to the protocol's character set: characters
 * outside it become spaces, and the result is cut at SNAPSHOT_TEXT_MAX.
 * `dst` must hold SNAPSHOT_TEXT_MAX + 1 bytes.
 */
void snapshot_clean_text(const char *src, char dst[SNAPSHOT_TEXT_MAX + 1]);

#endif /* SNAPSHOT_JSON_H */

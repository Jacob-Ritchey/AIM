#ifndef AIM3_H
#define AIM3_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "sha256.h"

/* ── Constants (must match Python source exactly) ───────────────────────── */

#define AIM3_MAGIC               "AIM3"
#define AIM3_VERSION             0x09
#define AIM3_VERSION_RECURSIVE   0x0A
#define AIM3_HEADER_SIZE         64
#define AIM3_GZ_LEVEL     9

#define BITSET_THRESHOLD  0.40

#define ANS_M_BITS  14
#define ANS_M       (1 << ANS_M_BITS)   /* 16384 */
#define ANS_L       ANS_M
#define ANS_B       256

#define MIN_CTX_O1       64
#define MIN_CTX_O2       128
#define DELTA_THRESHOLD  256
#define MIN_CODING_GAIN  6

/* Flag-stream encoding modes */
#define FLAG_MODE_GAP_GZ  0
#define FLAG_MODE_BITSET  1
#define FLAG_MODE_EF      2
#define FLAG_MODE_RLE     3

/* Aligned-data backend IDs */
#define BACKEND_GZIP       0
#define BACKEND_ANS0       1
#define BACKEND_ANS1       2
#define BACKEND_ANS2D      4
#define BACKEND_ANS_STRIDE 5   /* stride-k order-1 ANS; k auto-selected */

/* ── Flag bitset ────────────────────────────────────────────────────────── */

/*
 * Flags are stored as a compact bitset: uint8_t array of (n+7)/8 bytes.
 * Bit i (0-based) is set iff position i had the target bit set.
 * Callers pre-allocate with calloc(1,(n+7)/8) and free() after use.
 * This replaces the old FlagList (uint64_t pos[]) which grew to k×8 bytes
 * (k = flag count) and caused OOM on high-density bit planes.
 */

/* ── Run statistics ─────────────────────────────────────────────────────── */

typedef struct {
    size_t   input_bytes;
    int      selected_bit;
    double   bit_select_time_s;
    int      flag_mode;
    size_t   flag_bytes;
    double   flag_time_s;
    /* backend_size[id]: compressed bytes per backend; 0 = not tried.
     * Indexed directly by backend ID: 0=gzip,1=ans0,2=ans1,3=unused,4=ans2d,5=stride */
    size_t   backend_size[6];
    int      best_backend;
    uint8_t  stride_k;          /* first byte of stride payload when stride wins; else 0 */
    double   backend_time_s;
    int      used_recursive;
    int      n_layers;
    size_t   recursive_bytes;   /* full recursive container size */
    double   recursive_time_s;
    uint8_t  container_version;
    size_t   output_bytes;
    double   total_time_s;
} AIM3EncodeStats;

typedef struct {
    uint8_t  container_version;
    int      n_layers;          /* v0x0A only; else 0 */
    int      flag_mode;
    int      backend;
    size_t   orig_bytes;
    size_t   container_bytes;
    size_t   decoded_bytes;
    int      sha_ok;            /* 1 = verified+passed; 0 = not verified or failed */
    double   total_time_s;
} AIM3DecodeStats;

/* Accessors — return pointer to file-static storage; not thread-safe. */
const AIM3EncodeStats *aim3_encode_stats(void);
const AIM3DecodeStats *aim3_decode_stats(void);

/* ── Top-level API ──────────────────────────────────────────────────────── */

/*
 * encode() — compress `data` of length `n` into `out`.
 *
 *   target_bit : 0-7, or -1 for auto-select
 *   backend    : 0=gzip, 1=ans0, 2=ans1, 4=ans2d, -1=auto (try all)
 *   gz_level   : 1-9 (9 = best compression)
 *   verbose    : print per-backend sizes to stdout
 *   sample_cap : 0 = use full data for bit selection; >0 = use first N bytes
 *   src_path   : if non-NULL, stream I/O from this file (data may be NULL).
 *                When NULL, data must be a valid pointer (used by bench).
 *
 * Returns 0 on success, -1 on error.
 * On success, out->data must be freed by the caller with buf_free().
 */
int aim3_encode(const uint8_t *data, size_t n,
                int target_bit, int backend,
                int gz_level, int verbose, size_t sample_cap,
                const char *src_path,
                Buf *out);

/*
 * decode() — decompress a container produced by aim3_encode().
 *
 *   verify : if non-zero, check SHA-256 of the recovered data.
 *
 * Returns 0 on success, -1 on error (sets aim3_errmsg).
 * On success, out->data must be freed by the caller with buf_free().
 */
int aim3_decode(const uint8_t *container, size_t clen,
                int verify, Buf *out);

/* Last error message (thread-unsafe, single global). */
extern const char *aim3_errmsg;

#endif /* AIM3_H */

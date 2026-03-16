#include "gamma_rle.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal: run-list helper ──────────────────────────────────────────── */

typedef struct {
    uint64_t *data;
    size_t    len;
    size_t    cap;
} RunList;

static void rl_init(RunList *r) { r->data = NULL; r->len = 0; r->cap = 0; }

static int rl_push(RunList *r, uint64_t v)
{
    if (r->len == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 64;
        uint64_t *np = realloc(r->data, nc * sizeof(uint64_t));
        if (!np) return -1;
        r->data = np; r->cap = nc;
    }
    r->data[r->len++] = v;
    return 0;
}

static void rl_free(RunList *r)
{
    free(r->data); r->data = NULL; r->len = 0; r->cap = 0;
}

/* ── Elias gamma encode/decode ──────────────────────────────────────────── */

/*
 * Encode run lengths into Elias gamma codes, MSB-first packed into bytes.
 * For each n >= 1: k = floor(log2(n)), emit k zero bits then (k+1) bits of n.
 */
static int gamma_encode_runs(const RunList *runs, Buf *out)
{
    /* Count total bits first to pre-allocate. */
    size_t total_bits = 0;
    for (size_t i = 0; i < runs->len; i++) {
        uint64_t n = runs->data[i] < 1 ? 1 : runs->data[i];
        int k = 0;
        uint64_t tmp = n;
        while (tmp > 1) { tmp >>= 1; k++; }  /* k = floor(log2(n)) */
        total_bits += (size_t)(2 * k + 1);
    }
    size_t total_bytes = (total_bits + 7) / 8;
    if (buf_reserve(out, total_bytes) < 0) return -1;
    /* Zero the output region we'll be writing to. */
    if (total_bytes > 0) {
        memset(out->data + out->len, 0, total_bytes);
    }

    size_t bit_pos = out->len * 8; /* global bit index into out->data */
    /* Expand out->len now so buf_push paths won't stomp us. */
    out->len += total_bytes;

    for (size_t i = 0; i < runs->len; i++) {
        uint64_t n = runs->data[i] < 1 ? 1 : runs->data[i];
        int k = 0;
        uint64_t tmp = n;
        while (tmp > 1) { tmp >>= 1; k++; }

        /* k leading zero bits — already zero, just advance. */
        bit_pos += (size_t)k;

        /* Then k+1 bits of n, MSB first. */
        for (int shift = k; shift >= 0; shift--) {
            if ((n >> shift) & 1) {
                size_t byte_i = bit_pos / 8;
                int    bit_i  = 7 - (int)(bit_pos & 7);
                out->data[byte_i] |= (uint8_t)(1u << bit_i);
            }
            bit_pos++;
        }
    }
    return 0;
}

/*
 * Decode n_runs Elias gamma coded run lengths from packed bytes.
 */
static int gamma_decode_runs(const uint8_t *data, size_t data_len,
                             uint64_t n_runs, RunList *out)
{
    size_t bit_pos    = 0;
    size_t total_bits = data_len * 8;

    for (size_t r = 0; r < n_runs; r++) {
        /* Count leading zeros. */
        int k = 0;
        while (bit_pos < total_bits) {
            size_t byte_i = bit_pos >> 3;
            int    bit_i  = 7 - (int)(bit_pos & 7);
            int    bit    = (data[byte_i] >> bit_i) & 1;
            bit_pos++;
            if (bit == 0) {
                k++;
            } else {
                /* Found the leading 1 bit: value starts at 1, read k more bits. */
                uint64_t n = 1;
                for (int j = 0; j < k; j++) {
                    if (bit_pos >= total_bits) break;
                    byte_i = bit_pos >> 3;
                    bit_i  = 7 - (int)(bit_pos & 7);
                    n = (n << 1) | ((data[byte_i] >> bit_i) & 1);
                    bit_pos++;
                }
                if (rl_push(out, n) < 0) return -1;
                break;
            }
        }
    }
    return 0;
}

/* ── Build run-list from flag bitset ────────────────────────────────────── */

static int build_runs(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                      RunList *runs, int *first_bit_out)
{
    if (n == 0) { *first_bit_out = 0; return 0; }

    int first_bit = (int)(flag_bs[0] & 1);
    *first_bit_out = first_bit;

    int      cur = first_bit;
    uint64_t run = 0;

    for (uint64_t i = 0; i < n; i++) {
        int b = (int)((flag_bs[i >> 3] >> (i & 7)) & 1);
        if (b == cur) {
            run++;
        } else {
            if (rl_push(runs, run) < 0) return -1;
            cur = b;
            run = 1;
        }
    }
    if (run > 0) {
        if (rl_push(runs, run) < 0) return -1;
    }
    (void)flag_bs_len;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int rle_encode(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n, Buf *out)
{
    RunList runs; rl_init(&runs);
    int first_bit = 0;

    if (build_runs(flag_bs, flag_bs_len, n, &runs, &first_bit) < 0) {
        rl_free(&runs); return -1;
    }

    /* Header: first_bit (1 byte) + n_runs (8 bytes uint64 BE) = 9 bytes. */
    uint8_t hdr[9];
    hdr[0] = (uint8_t)first_bit;
    uint64_t nr = (uint64_t)runs.len;
    hdr[1]=(uint8_t)(nr>>56); hdr[2]=(uint8_t)(nr>>48);
    hdr[3]=(uint8_t)(nr>>40); hdr[4]=(uint8_t)(nr>>32);
    hdr[5]=(uint8_t)(nr>>24); hdr[6]=(uint8_t)(nr>>16);
    hdr[7]=(uint8_t)(nr>> 8); hdr[8]=(uint8_t)nr;
    if (buf_append(out, hdr, 9) < 0) { rl_free(&runs); return -1; }

    int ret = gamma_encode_runs(&runs, out);
    rl_free(&runs);
    return ret;
}

int rle_decode(const uint8_t *data, size_t data_len, uint64_t n,
               uint8_t *flag_bs, size_t flag_bs_len)
{
    if (data_len < 9) return -1;
    int      first_bit = data[0];
    uint64_t n_runs    = ((uint64_t)data[1]<<56)|((uint64_t)data[2]<<48)|
                         ((uint64_t)data[3]<<40)|((uint64_t)data[4]<<32)|
                         ((uint64_t)data[5]<<24)|((uint64_t)data[6]<<16)|
                         ((uint64_t)data[7]<< 8)| (uint64_t)data[8];

    const uint8_t *gamma_data     = data + 9;
    size_t         gamma_data_len = data_len - 9;

    RunList runs; rl_init(&runs);
    if (gamma_decode_runs(gamma_data, gamma_data_len, n_runs, &runs) < 0) {
        rl_free(&runs); return -1;
    }

    uint64_t pos = 0;
    int cur_bit = first_bit;
    for (size_t i = 0; i < runs.len; i++) {
        uint64_t run = runs.data[i];
        if (cur_bit == 1) {
            for (uint64_t j = 0; j < run; j++) {
                uint64_t p = pos + j;
                if (p < n && p < flag_bs_len * 8)
                    flag_bs[p >> 3] |= (uint8_t)(1u << (p & 7));
            }
        }
        pos += run;
        cur_bit ^= 1;
    }

    rl_free(&runs);
    return 0;
}

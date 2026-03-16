#include "elias_fano.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── LSB-first bit helpers ──────────────────────────────────────────────── */

/* Write `bits_left` bits of `val` (LSB first) starting at bit offset `bo`
 * into byte array `arr` of length `arr_len`. */
static void write_bits_lsb(uint8_t *arr, size_t arr_len,
                            size_t bo, uint64_t val, int bits_left)
{
    while (bits_left > 0) {
        size_t bi    = bo >> 3;
        int    bb    = (int)(bo & 7);
        int    chunk = bits_left < (8 - bb) ? bits_left : (8 - bb);
        if (bi >= arr_len) break;
        arr[bi] |= (uint8_t)((val & ((1u << chunk) - 1u)) << bb);
        val      >>= chunk;
        bo       += (size_t)chunk;
        bits_left -= chunk;
    }
}

/* Read `bits_left` bits (LSB first) from byte array starting at bit offset `bo`. */
static uint64_t read_bits_lsb(const uint8_t *arr, size_t arr_len,
                               size_t bo, int bits_left)
{
    uint64_t val = 0; int shift = 0;
    while (bits_left > 0) {
        size_t bi    = bo >> 3;
        int    bb    = (int)(bo & 7);
        int    chunk = bits_left < (8 - bb) ? bits_left : (8 - bb);
        if (bi >= arr_len) break;
        uint8_t bv = arr[bi];
        val      |= (uint64_t)(((bv >> bb) & ((1u << chunk) - 1u))) << shift;
        shift    += chunk;
        bo       += (size_t)chunk;
        bits_left -= chunk;
    }
    return val;
}

/* ── ef_encode ──────────────────────────────────────────────────────────── */

int ef_encode(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t N, Buf *out)
{
    /* Count set bits (k). */
    uint64_t k = 0;
    for (size_t i = 0; i < flag_bs_len; i++) {
        uint8_t b = flag_bs[i];
        while (b) { k++; b &= b - 1; }
    }

    /* Header: N (8 bytes) + k (8 bytes) + l (1 byte) = 17 bytes. */
    uint8_t hdr[17];
    hdr[0]=(uint8_t)(N>>56); hdr[1]=(uint8_t)(N>>48);
    hdr[2]=(uint8_t)(N>>40); hdr[3]=(uint8_t)(N>>32);
    hdr[4]=(uint8_t)(N>>24); hdr[5]=(uint8_t)(N>>16);
    hdr[6]=(uint8_t)(N>> 8); hdr[7]=(uint8_t)N;
    hdr[8] =(uint8_t)(k>>56); hdr[9] =(uint8_t)(k>>48);
    hdr[10]=(uint8_t)(k>>40); hdr[11]=(uint8_t)(k>>32);
    hdr[12]=(uint8_t)(k>>24); hdr[13]=(uint8_t)(k>>16);
    hdr[14]=(uint8_t)(k>> 8); hdr[15]=(uint8_t)k;

    if (k == 0) {
        hdr[16] = 0;
        return buf_append(out, hdr, 17);
    }

    /* l = min(max(0, floor(log2(N/k))), 30) */
    int l = 0;
    if (N > k) {
        double ratio = (double)N / (double)k;
        l = (int)floor(log2(ratio));
        if (l < 0)  l = 0;
        if (l > 30) l = 30;
    }
    hdr[16] = (uint8_t)l;
    if (buf_append(out, hdr, 17) < 0) return -1;

    /* Lower bits: k * l bits, LSB-first — iterate bitset for positions. */
    size_t lower_bits  = (size_t)k * (size_t)l;
    size_t lower_bytes = (lower_bits + 7) / 8;

    uint8_t *lower_buf = NULL;
    if (lower_bytes > 0) {
        lower_buf = calloc(lower_bytes, 1);
        if (!lower_buf) return -1;
        uint64_t low_mask = (l == 0) ? 0 : (((uint64_t)1 << l) - 1);
        size_t ei = 0;
        for (uint64_t i = 0; i < N && ei < (size_t)k; i++) {
            if (!((flag_bs[i >> 3] >> (i & 7)) & 1)) continue;
            uint64_t low_val = i & low_mask;
            write_bits_lsb(lower_buf, lower_bytes, ei * (size_t)l, low_val, l);
            ei++;
        }
        if (buf_append(out, lower_buf, lower_bytes) < 0) {
            free(lower_buf); return -1;
        }
        free(lower_buf);
    }

    /* Upper bits: unary buckets, LSB-first — iterate bitset. */
    size_t upper_size  = (size_t)k + (size_t)(N >> l) + 1;
    size_t upper_bytes = (upper_size + 7) / 8;

    uint8_t *upper_buf = calloc(upper_bytes, 1);
    if (!upper_buf) return -1;

    size_t   bit_pos = 0;
    uint64_t bucket  = 0;
    for (uint64_t i = 0; i < N; i++) {
        if (!((flag_bs[i >> 3] >> (i & 7)) & 1)) continue;
        uint64_t cur_bucket = (uint64_t)(i >> l);
        /* Emit separator 0-bits for skipped buckets. */
        while (bucket < cur_bucket) { bit_pos++; bucket++; }
        /* Emit 1-bit for this element. */
        size_t bi = bit_pos >> 3;
        int    bb = (int)(bit_pos & 7);
        if (bi < upper_bytes)
            upper_buf[bi] |= (uint8_t)(1u << bb);
        bit_pos++;
    }

    size_t used_bytes = (bit_pos + 7) / 8;
    int ret = buf_append(out, upper_buf, used_bytes);
    free(upper_buf);
    return ret;
}

/* ── ef_decode ──────────────────────────────────────────────────────────── */

int ef_decode(const uint8_t *data, size_t data_len,
              uint8_t *flag_bs, size_t flag_bs_len)
{
    if (data_len < 17) return -1;

    /* N stored in header; not needed during decode. */
    (void)(((uint64_t)data[0]<<56)|((uint64_t)data[1]<<48)|
           ((uint64_t)data[2]<<40)|((uint64_t)data[3]<<32)|
           ((uint64_t)data[4]<<24)|((uint64_t)data[5]<<16)|
           ((uint64_t)data[6]<< 8)| (uint64_t)data[7]);
    uint64_t k = ((uint64_t)data[8]<<56)|((uint64_t)data[9]<<48)|
                 ((uint64_t)data[10]<<40)|((uint64_t)data[11]<<32)|
                 ((uint64_t)data[12]<<24)|((uint64_t)data[13]<<16)|
                 ((uint64_t)data[14]<< 8)| (uint64_t)data[15];
    int l = (int)data[16];

    if (k == 0) return 0;

    size_t lower_byte_count = (l > 0) ? ((size_t)k * (size_t)l + 7) / 8 : 0;
    if (17 + lower_byte_count > data_len) return -1;

    const uint8_t *lower_bytes = data + 17;
    const uint8_t *upper_bytes = data + 17 + lower_byte_count;
    size_t         upper_len   = data_len - 17 - lower_byte_count;

    /* Decode lower values. */
    uint64_t *lower_vals = calloc((size_t)k, sizeof(uint64_t));
    if (!lower_vals) return -1;
    if (l > 0) {
        for (uint64_t i = 0; i < k; i++) {
            lower_vals[i] = read_bits_lsb(lower_bytes, lower_byte_count,
                                          (size_t)i * (size_t)l, l);
        }
    }

    /* Decode upper (unary) — set bits in flag_bs. */
    uint64_t bucket = 0;
    uint64_t found  = 0;
    size_t   bit_pos = 0;
    size_t   upper_total = upper_len * 8;

    while (found < k && bit_pos < upper_total) {
        size_t bi = bit_pos >> 3;
        int    bb = (int)(bit_pos & 7);
        if (bi >= upper_len) break;
        if ((upper_bytes[bi] >> bb) & 1) {
            uint64_t pos = (bucket << l) | lower_vals[found];
            if (pos < flag_bs_len * 8)
                flag_bs[pos >> 3] |= (uint8_t)(1u << (pos & 7));
            found++;
        } else {
            bucket++;
        }
        bit_pos++;
    }

    free(lower_vals);
    return 0;
}

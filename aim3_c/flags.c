#include "flags.h"
#include "gamma_rle.h"
#include "elias_fano.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ── zlib helpers ───────────────────────────────────────────────────────── */

static int gz_compress(const uint8_t *src, size_t src_len,
                       int level, Buf *out)
{
    /* Upper bound for gzip output. */
    uLongf bound = compressBound((uLong)src_len) + 64;
    if (buf_reserve(out, out->len + bound) < 0) return -1;

    /* Use deflate with gzip wrapper (windowBits = 15 + 16). */
    z_stream zs = {0};
    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;

    zs.next_out  = out->data + out->len;
    zs.avail_out = (uInt)bound;
    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) return -1;

    out->len += zs.total_out;
    return 0;
}

static int gz_decompress(const uint8_t *src, size_t src_len, Buf *out)
{
    z_stream zs = {0};
    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return -1;

    size_t chunk = 65536;
    int zret;
    do {
        if (buf_reserve(out, out->len + chunk) < 0) { inflateEnd(&zs); return -1; }
        zs.next_out  = out->data + out->len;
        zs.avail_out = (uInt)chunk;
        zret = inflate(&zs, Z_NO_FLUSH);
        if (zret == Z_STREAM_ERROR || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR) {
            inflateEnd(&zs); return -1;
        }
        out->len += chunk - zs.avail_out;
    } while (zret != Z_STREAM_END);

    inflateEnd(&zs);
    return 0;
}

/* ── encode_flags_gz ────────────────────────────────────────────────────── */

int encode_flags_gz(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                    int gz_level, Buf *out, int *mode_out)
{
    /* Count set bits (k) and compute density. */
    uint64_t k = 0;
    for (size_t i = 0; i < flag_bs_len; i++) {
        uint8_t b = flag_bs[i]; while (b) { k++; b &= b - 1; }
    }
    double density = n ? (double)k / (double)n : 0.0;

    if (density >= BITSET_THRESHOLD || k == 0) {
        /* Bitset mode: gzip the flag_bs directly. */
        int ret = gz_compress(flag_bs, flag_bs_len, gz_level, out);
        *mode_out = FLAG_MODE_BITSET;
        return ret;
    }

    /* Gap mode: two passes over bitset — first to find max_gap, second to emit. */
    uint64_t max_gap = 0, prev = 0;
    int first = 1;
    for (uint64_t i = 0; i < n; i++) {
        if ((flag_bs[i >> 3] >> (i & 7)) & 1) {
            uint64_t gap = first ? i : i - prev;
            if (gap > max_gap) max_gap = gap;
            prev = i; first = 0;
        }
    }

    Buf raw; buf_init(&raw);
    int ret = 0;

    if (max_gap <= 255) {
        uint32_t kk = (uint32_t)k;
        uint8_t  hdr[4] = {(uint8_t)(kk>>24),(uint8_t)(kk>>16),
                           (uint8_t)(kk>>8),(uint8_t)kk};
        if (buf_append(&raw, hdr, 4) < 0) { ret = -1; goto done; }
        prev = 0; first = 1;
        for (uint64_t i = 0; i < n; i++) {
            if (!((flag_bs[i >> 3] >> (i & 7)) & 1)) continue;
            uint64_t gap = first ? i : i - prev;
            uint8_t g = (uint8_t)gap;
            if (buf_push(&raw, g) < 0) { ret = -1; goto done; }
            prev = i; first = 0;
        }
    } else if (max_gap <= 65534) {
        uint32_t kk = (uint32_t)k | 0x80000000u;
        uint8_t  hdr[4] = {(uint8_t)(kk>>24),(uint8_t)(kk>>16),
                           (uint8_t)(kk>>8),(uint8_t)kk};
        if (buf_append(&raw, hdr, 4) < 0) { ret = -1; goto done; }
        prev = 0; first = 1;
        for (uint64_t i = 0; i < n; i++) {
            if (!((flag_bs[i >> 3] >> (i & 7)) & 1)) continue;
            uint64_t gap = first ? i : i - prev;
            uint16_t gw = (uint16_t)gap;
            uint8_t  gb[2] = {(uint8_t)(gw >> 8), (uint8_t)gw};
            if (buf_append(&raw, gb, 2) < 0) { ret = -1; goto done; }
            prev = i; first = 0;
        }
    } else {
        uint32_t kk = (uint32_t)k | 0x80000000u | 0x40000000u;
        uint8_t  hdr[4] = {(uint8_t)(kk>>24),(uint8_t)(kk>>16),
                           (uint8_t)(kk>>8),(uint8_t)kk};
        if (buf_append(&raw, hdr, 4) < 0) { ret = -1; goto done; }
        prev = 0; first = 1;
        for (uint64_t i = 0; i < n; i++) {
            if (!((flag_bs[i >> 3] >> (i & 7)) & 1)) continue;
            uint64_t gap = first ? i : i - prev;
            while (gap > 65534) {
                uint8_t sent[2] = {0xFF, 0xFF};
                if (buf_append(&raw, sent, 2) < 0) { ret = -1; goto done; }
                gap -= 65534;
            }
            uint16_t gw = (uint16_t)gap;
            uint8_t  gb[2] = {(uint8_t)(gw >> 8), (uint8_t)gw};
            if (buf_append(&raw, gb, 2) < 0) { ret = -1; goto done; }
            prev = i; first = 0;
        }
    }

    ret = gz_compress(raw.data, raw.len, gz_level, out);
    *mode_out = FLAG_MODE_GAP_GZ;

done:
    buf_free(&raw);
    return ret;
}

/* ── encode_flags_ef ────────────────────────────────────────────────────── */

int encode_flags_ef(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                    Buf *out, int *mode_out)
{
    *mode_out = FLAG_MODE_EF;
    return ef_encode(flag_bs, flag_bs_len, n, out);
}

/* ── encode_flags_rle ───────────────────────────────────────────────────── */

int encode_flags_rle(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                     Buf *out, int *mode_out)
{
    *mode_out = FLAG_MODE_RLE;
    return rle_encode(flag_bs, flag_bs_len, n, out);
}

/* ── encode_flags_best ──────────────────────────────────────────────────── */

int encode_flags_best(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                      int gz_level, Buf *out, int *mode_out)
{
    Buf b_gz, b_ef, b_rle;
    int m_gz, m_ef, m_rle;
    buf_init(&b_gz); buf_init(&b_ef); buf_init(&b_rle);

    int ok = 0;
    if (encode_flags_gz(flag_bs, flag_bs_len, n, gz_level, &b_gz, &m_gz)  < 0) { ok = -1; goto done; }
    if (encode_flags_ef(flag_bs, flag_bs_len, n, &b_ef, &m_ef)            < 0) { ok = -1; goto done; }
    if (encode_flags_rle(flag_bs, flag_bs_len, n, &b_rle, &m_rle)         < 0) { ok = -1; goto done; }

    /* Pick smallest. */
    Buf *best; int best_mode;
    if (b_gz.len <= b_ef.len && b_gz.len <= b_rle.len) {
        best = &b_gz; best_mode = m_gz;
    } else if (b_ef.len <= b_rle.len) {
        best = &b_ef; best_mode = m_ef;
    } else {
        best = &b_rle; best_mode = m_rle;
    }

    if (buf_append(out, best->data, best->len) < 0) { ok = -1; goto done; }
    *mode_out = best_mode;

done:
    buf_free(&b_gz); buf_free(&b_ef); buf_free(&b_rle);
    return ok;
}

/* ── decode_flags ───────────────────────────────────────────────────────── */

int decode_flags(const uint8_t *payload, size_t payload_len,
                 uint64_t n, int flag_mode,
                 uint8_t *flag_bs, size_t flag_bs_len)
{
    if (flag_mode == FLAG_MODE_RLE) {
        return rle_decode(payload, payload_len, n, flag_bs, flag_bs_len);
    }

    if (flag_mode == FLAG_MODE_EF) {
        return ef_decode(payload, payload_len, flag_bs, flag_bs_len);
    }

    if (flag_mode == FLAG_MODE_BITSET) {
        /* Decompress directly into flag_bs (already zeroed by caller). */
        Buf raw; buf_init(&raw);
        if (gz_decompress(payload, payload_len, &raw) < 0) {
            buf_free(&raw); return -1;
        }
        size_t copy = raw.len < flag_bs_len ? raw.len : flag_bs_len;
        for (size_t i = 0; i < copy; i++) flag_bs[i] |= raw.data[i];
        buf_free(&raw);
        return 0;
    }

    /* FLAG_MODE_GAP_GZ */
    Buf raw; buf_init(&raw);
    if (gz_decompress(payload, payload_len, &raw) < 0) {
        buf_free(&raw); return -1;
    }
    if (raw.len < 4) { buf_free(&raw); return -1; }

    uint32_t raw_k = ((uint32_t)raw.data[0] << 24) | ((uint32_t)raw.data[1] << 16)
                   | ((uint32_t)raw.data[2] <<  8) |  (uint32_t)raw.data[3];

    int sentinel_mode = (raw_k & 0x40000000u) != 0;
    int two_byte      = (raw_k & 0x80000000u) != 0;
    uint32_t k = raw_k & 0x3FFFFFFFu;

    if (k == 0) { buf_free(&raw); return 0; }

    size_t pos = 4;
    uint64_t cur = 0;

#define SET_FLAG(p) do { \
    uint64_t _p = (p); \
    if (_p < flag_bs_len * 8) flag_bs[_p >> 3] |= (uint8_t)(1u << (_p & 7)); \
} while(0)

    if (!two_byte) {
        for (uint32_t i = 0; i < k; i++) {
            if (pos >= raw.len) { buf_free(&raw); return -1; }
            cur += raw.data[pos++];
            SET_FLAG(cur);
        }
    } else if (!sentinel_mode) {
        for (uint32_t i = 0; i < k; i++) {
            if (pos + 1 >= raw.len) { buf_free(&raw); return -1; }
            uint16_t g = ((uint16_t)raw.data[pos] << 8) | raw.data[pos + 1];
            pos += 2; cur += g;
            SET_FLAG(cur);
        }
    } else {
        for (uint32_t i = 0; i < k; i++) {
            uint64_t gap = 0;
            while (1) {
                if (pos + 1 >= raw.len) { buf_free(&raw); return -1; }
                uint16_t g = ((uint16_t)raw.data[pos] << 8) | raw.data[pos + 1];
                pos += 2;
                if (g == 0xFFFF) { gap += 65534; }
                else { gap += g; break; }
            }
            cur += gap;
            SET_FLAG(cur);
        }
    }
#undef SET_FLAG

    buf_free(&raw);
    return 0;
}

#include "bit_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── bit_clear ──────────────────────────────────────────────────────────── */

int bit_clear(const uint8_t *data, size_t n, int bit,
              Buf *aligned_out, uint8_t *flag_bs)
{
    uint8_t mask = (uint8_t)(1u << bit);
    uint8_t inv  = (uint8_t)(~mask & 0xFF);

    if (buf_reserve_exact(aligned_out, n) < 0) return -1;
    aligned_out->len = n;

    for (size_t i = 0; i < n; i++) {
        uint8_t v = data[i];
        if (v & mask) {
            aligned_out->data[i] = v & inv;
            flag_bs[i >> 3] |= (uint8_t)(1u << (i & 7));
        } else {
            aligned_out->data[i] = v;
        }
    }
    return 0;
}

/* ── bit_clear_stream ───────────────────────────────────────────────────── */

int bit_clear_stream(const char *path, size_t n, int bit,
                     Buf *aligned_out, uint8_t *flag_bs)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (buf_reserve_exact(aligned_out, n) < 0) { fclose(f); return -1; }
    aligned_out->len = 0;

    uint8_t chunk[65536];
    uint8_t mask = (uint8_t)(1u << bit);
    uint8_t inv  = (uint8_t)(~mask & 0xFF);
    uint64_t pos = 0;
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            uint8_t v = chunk[i];
            uint64_t p = pos + (uint64_t)i;
            if (v & mask) {
                flag_bs[p >> 3] |= (uint8_t)(1u << (p & 7));
                aligned_out->data[aligned_out->len++] = v & inv;
            } else {
                aligned_out->data[aligned_out->len++] = v;
            }
        }
        pos += (uint64_t)got;
    }
    fclose(f);
    return 0;
}

/* ── reconstruct ────────────────────────────────────────────────────────── */

void reconstruct(const uint8_t *aligned, size_t n,
                 const uint8_t *flag_bs, int bit,
                 uint8_t *result)
{
    uint8_t mask = (uint8_t)(1u << bit);
    memcpy(result, aligned, n);
    for (size_t i = 0; i < n; i++) {
        if ((flag_bs[i >> 3] >> (i & 7)) & 1)
            result[i] |= mask;
    }
}

/* ── remap_to_128 ───────────────────────────────────────────────────────── */

int remap_to_128(const uint8_t *aligned, size_t n, int bit, Buf *out)
{
    uint8_t lo     = (uint8_t)((1u << bit) - 1);
    uint8_t hi_xor = (uint8_t)(0x7F ^ lo);

    if (buf_reserve_exact(out, n) < 0) return -1;
    out->len = n;

    for (size_t i = 0; i < n; i++) {
        uint8_t v = aligned[i];
        out->data[i] = (uint8_t)((v & lo) | ((v >> 1) & hi_xor));
    }
    return 0;
}

/* ── unmap_from_128 ─────────────────────────────────────────────────────── */

int unmap_from_128(const uint8_t *syms, size_t n, int bit, Buf *out)
{
    uint8_t lo      = (uint8_t)((1u << bit) - 1);
    uint8_t hi_mask = (uint8_t)(0x7F ^ lo);

    if (buf_reserve_exact(out, n) < 0) return -1;
    out->len = n;

    for (size_t i = 0; i < n; i++) {
        uint8_t s = syms[i];
        out->data[i] = (uint8_t)((s & lo) | ((s & hi_mask) << 1));
    }
    return 0;
}

#include "recursive.h"
#include "aim3.h"
#include "bit_utils.h"
#include "flags.h"
#include "ans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Layer wire format (19 bytes fixed header) ───────────────────────────
 *
 *  [0]    bit_stripped   uint8   (0-7)
 *  [1]    flag_mode      uint8   (FLAG_MODE_*)
 *  [2-9]  flag_len       uint64 BE
 *  [10]   halt_code      uint8
 *  [11-18]child_len      uint64 BE
 *  [19..19+flag_len-1]   flag_block
 *  [19+flag_len..19+flag_len+child_len-1]  child
 *
 * ─────────────────────────────────────────────────────────────────────── */

#define LAYER_HDR  19   /* fixed header size per layer */

/* ── encode_layer ────────────────────────────────────────────────────── */

static int encode_layer(const uint8_t *data, size_t n, int depth,
                        Buf *parent_aligned_to_free,
                        int *flag_mode_out, Buf *out)
{
    /* 1. Pick the sparsest bit plane. */
    size_t best_count = (size_t)-1;
    int    best_bit   = 0;
    for (int b = 0; b < 8; b++) {
        size_t cnt = 0;
        uint8_t mask = (uint8_t)(1u << b);
        for (size_t i = 0; i < n; i++) if (data[i] & mask) cnt++;
        if (cnt < best_count) { best_count = cnt; best_bit = b; }
    }

    /* 2. Extract flag bitset and aligned residual. */
    Buf      aligned; buf_init(&aligned);
    size_t   flag_bs_len = (n + 7) / 8;
    uint8_t *flag_bs = calloc(1, flag_bs_len);
    if (!flag_bs) return -1;
    if (bit_clear(data, n, best_bit, &aligned, flag_bs) < 0) {
        buf_free(&aligned); free(flag_bs); return -1;
    }
    /* Parent's aligned buffer is fully consumed — free it immediately to
     * avoid accumulating N-byte aligned buffers across all recursion depths. */
    if (parent_aligned_to_free) { buf_free(parent_aligned_to_free); }

    /* 3. Encode flags (best of gz/EF/RLE). Pre-size to flag_bs_len (upper bound). */
    Buf flag_block; buf_init(&flag_block);
    (void)buf_reserve_exact(&flag_block, flag_bs_len);
    int flag_mode;
    if (encode_flags_best(flag_bs, flag_bs_len, (uint64_t)n, AIM3_GZ_LEVEL,
                          &flag_block, &flag_mode) < 0) {
        buf_free(&aligned); free(flag_bs); buf_free(&flag_block); return -1;
    }
    free(flag_bs);
    if (flag_mode_out) *flag_mode_out = flag_mode;

    /* 4. Remap aligned to [0,127]. */
    Buf syms; buf_init(&syms);
    if (remap_to_128(aligned.data, aligned.len, best_bit, &syms) < 0) {
        buf_free(&aligned); buf_free(&flag_block); buf_free(&syms); return -1;
    }

    /* 5. Check trivial cases. */
    int all_zero = 1, all_same = 1;
    uint8_t first = syms.len > 0 ? syms.data[0] : 0;
    for (size_t i = 0; i < syms.len && (all_zero || all_same); i++) {
        if (syms.data[i] != 0)     all_zero = 0;
        if (syms.data[i] != first) all_same = 0;
    }

    /* 6. Try ANS-stride on the remapped residual.
     * Pre-size ans_buf to syms.len to avoid exponential doubling on large inputs. */
    Buf ans_buf; buf_init(&ans_buf);
    (void)buf_reserve_exact(&ans_buf, syms.len);
    int ans_ok = (syms.len > 0 && !all_zero && !all_same)
               ? ans_stride_encode(syms.data, syms.len, &ans_buf)
               : -1;
    buf_free(&syms);  /* syms consumed; free before recursing to reduce peak */

    /* 7. Try recursing one more layer (on the aligned bytes, not syms).
     * Pass &aligned so the child frees it after bit_clear consumes it. */
    Buf child_buf; buf_init(&child_buf);
    int child_flag_mode = 0;
    int can_recurse = (depth + 1 < MAX_RECURSIVE_LAYERS)
                   && !all_zero && !all_same && aligned.len >= 8;
    if (can_recurse) {
        encode_layer(aligned.data, aligned.len, depth + 1,
                     &aligned,  /* child frees this after its bit_clear */
                     &child_flag_mode, &child_buf);
    }
    buf_free(&aligned);  /* no-op if child already freed it via parent_aligned_to_free */

    /* 8. Choose halt code and payload. */
    uint8_t halt;
    Buf *child_payload;

    if (all_zero || all_same) {
        halt = HALT_ZERO; child_payload = NULL;
    } else if (can_recurse && child_buf.len > 0 &&
               (ans_ok < 0 || child_buf.len < ans_buf.len + LAYER_HDR)) {
        halt = HALT_RECURSE; child_payload = &child_buf;
    } else if (ans_ok == 0) {
        halt = HALT_ANS; child_payload = &ans_buf;
    } else {
        halt = HALT_ANS; child_payload = &ans_buf;
    }

    /* 9. Emit wire format:
     *   [0]    bit_stripped
     *   [1]    flag_mode
     *   [2-9]  flag_len  uint64 BE
     *   [10]   halt
     *   [11-18]child_len uint64 BE
     *   [19..] flag_block + child
     */
    uint8_t hdr[LAYER_HDR];
    hdr[0] = (uint8_t)best_bit;
    hdr[1] = (uint8_t)flag_mode;
    uint64_t fl_len = (uint64_t)flag_block.len;
    hdr[2]=(uint8_t)(fl_len>>56); hdr[3]=(uint8_t)(fl_len>>48);
    hdr[4]=(uint8_t)(fl_len>>40); hdr[5]=(uint8_t)(fl_len>>32);
    hdr[6]=(uint8_t)(fl_len>>24); hdr[7]=(uint8_t)(fl_len>>16);
    hdr[8]=(uint8_t)(fl_len>> 8); hdr[9]=(uint8_t)fl_len;
    hdr[10] = halt;
    uint64_t clen = child_payload ? (uint64_t)child_payload->len : 0;
    hdr[11]=(uint8_t)(clen>>56); hdr[12]=(uint8_t)(clen>>48);
    hdr[13]=(uint8_t)(clen>>40); hdr[14]=(uint8_t)(clen>>32);
    hdr[15]=(uint8_t)(clen>>24); hdr[16]=(uint8_t)(clen>>16);
    hdr[17]=(uint8_t)(clen>> 8); hdr[18]=(uint8_t)clen;

    int ret = 0;
    if (buf_append(out, hdr, LAYER_HDR) < 0) ret = -1;
    if (ret == 0 && buf_append(out, flag_block.data, flag_block.len) < 0) ret = -1;
    if (ret == 0 && child_payload && child_payload->len > 0)
        if (buf_append(out, child_payload->data, child_payload->len) < 0)
            ret = -1;

    /* aligned and syms freed early above; buf_free on zeroed Buf is a no-op */
    buf_free(&flag_block); buf_free(&ans_buf); buf_free(&child_buf);
    return ret;
}

/* ── recursive_encode ───────────────────────────────────────────────── */

int recursive_encode(const uint8_t *data, size_t n,
                     Buf *out,
                     int *n_layers_out, int *flag_mode_out)
{
    int outer_flag_mode = 0;
    int ret = encode_layer(data, n, 0, NULL, &outer_flag_mode, out);
    if (ret < 0) return -1;

    /* Count layers by walking the halt chain. */
    int layers = 0;
    size_t pos = 0;
    while (pos + LAYER_HDR <= out->len) {
        layers++;
        uint64_t fl_len =
            ((uint64_t)out->data[pos+2]<<56)|((uint64_t)out->data[pos+3]<<48)|
            ((uint64_t)out->data[pos+4]<<40)|((uint64_t)out->data[pos+5]<<32)|
            ((uint64_t)out->data[pos+6]<<24)|((uint64_t)out->data[pos+7]<<16)|
            ((uint64_t)out->data[pos+8]<< 8)|(uint64_t)out->data[pos+9];
        uint8_t halt = out->data[pos+10];
        uint64_t child_len =
            ((uint64_t)out->data[pos+11]<<56)|((uint64_t)out->data[pos+12]<<48)|
            ((uint64_t)out->data[pos+13]<<40)|((uint64_t)out->data[pos+14]<<32)|
            ((uint64_t)out->data[pos+15]<<24)|((uint64_t)out->data[pos+16]<<16)|
            ((uint64_t)out->data[pos+17]<< 8)|(uint64_t)out->data[pos+18];
        if (halt != HALT_RECURSE) break;
        pos += LAYER_HDR + fl_len + child_len;
    }

    if (n_layers_out)  *n_layers_out  = layers;
    if (flag_mode_out) *flag_mode_out = outer_flag_mode;
    return 0;
}

/* ── encode_layer_stream ────────────────────────────────────────────────
 *
 * Streaming depth-0 variant of encode_layer: uses a popcount pass to pick
 * the sparsest bit and bit_clear_stream to produce the aligned buffer,
 * avoiding any N-byte input allocation.  Depth 1+ recurse into the normal
 * encode_layer (in-memory residuals only).
 * ─────────────────────────────────────────────────────────────────────── */

static int encode_layer_stream(const char *src_path, size_t n,
                               int *flag_mode_out, Buf *out)
{
    /* 1. Pick the sparsest bit via a single streaming popcount pass. */
    FILE *f = fopen(src_path, "rb");
    if (!f) return -1;
    uint64_t cnt[8] = {0};
    uint8_t  scan[65536];
    size_t   got;
    while ((got = fread(scan, 1, sizeof(scan), f)) > 0)
        for (size_t i = 0; i < got; i++)
            for (int b = 0; b < 8; b++)
                if (scan[i] & (1u << b)) cnt[b]++;
    fclose(f);

    int    best_bit   = 0;
    size_t best_count = (size_t)cnt[0];
    for (int b = 1; b < 8; b++)
        if ((size_t)cnt[b] < best_count) { best_count = (size_t)cnt[b]; best_bit = b; }

    /* 2. Stream bit_clear — produces aligned buffer without an N-byte input copy. */
    Buf      aligned; buf_init(&aligned);
    size_t   flag_bs_len = (n + 7) / 8;
    uint8_t *flag_bs = calloc(1, flag_bs_len);
    if (!flag_bs) return -1;
    if (bit_clear_stream(src_path, n, best_bit, &aligned, flag_bs) < 0) {
        buf_free(&aligned); free(flag_bs); return -1;
    }

    /* 3. Encode flags (best of gz/EF/RLE). Pre-size to flag_bs_len (upper bound). */
    Buf flag_block; buf_init(&flag_block);
    (void)buf_reserve_exact(&flag_block, flag_bs_len);
    int flag_mode;
    if (encode_flags_best(flag_bs, flag_bs_len, (uint64_t)n, AIM3_GZ_LEVEL,
                          &flag_block, &flag_mode) < 0) {
        buf_free(&aligned); free(flag_bs); buf_free(&flag_block); return -1;
    }
    free(flag_bs);
    if (flag_mode_out) *flag_mode_out = flag_mode;

    /* 4. Remap aligned to [0,127]. */
    Buf syms; buf_init(&syms);
    if (remap_to_128(aligned.data, aligned.len, best_bit, &syms) < 0) {
        buf_free(&aligned); buf_free(&flag_block); buf_free(&syms); return -1;
    }

    /* 5. Check trivial cases. */
    int all_zero = 1, all_same = 1;
    uint8_t first = syms.len > 0 ? syms.data[0] : 0;
    for (size_t i = 0; i < syms.len && (all_zero || all_same); i++) {
        if (syms.data[i] != 0)     all_zero = 0;
        if (syms.data[i] != first) all_same = 0;
    }

    /* 6. Try ANS-stride on the remapped residual.
     * Pre-size ans_buf to syms.len to avoid exponential doubling on large inputs. */
    Buf ans_buf; buf_init(&ans_buf);
    (void)buf_reserve_exact(&ans_buf, syms.len);
    int ans_ok = (syms.len > 0 && !all_zero && !all_same)
               ? ans_stride_encode(syms.data, syms.len, &ans_buf)
               : -1;
    buf_free(&syms);  /* syms consumed; free before recursing to reduce peak */

    /* 7. Try recursing one more layer (on aligned bytes, not syms).
     *    From depth 1 onward the residual is in-memory; use encode_layer.
     *    Pass &aligned so depth-1 encode_layer frees it after its bit_clear. */
    Buf child_buf; buf_init(&child_buf);
    int child_flag_mode = 0;
    int can_recurse = (1 < MAX_RECURSIVE_LAYERS)  /* depth 0 → depth 1 */
                   && !all_zero && !all_same && aligned.len >= 8;
    if (can_recurse) {
        encode_layer(aligned.data, aligned.len, 1,
                     &aligned,  /* child frees this after its bit_clear */
                     &child_flag_mode, &child_buf);
    }
    buf_free(&aligned);  /* no-op if child already freed it via parent_aligned_to_free */

    /* 8. Choose halt code and payload. */
    uint8_t halt;
    Buf *child_payload;

    if (all_zero || all_same) {
        halt = HALT_ZERO; child_payload = NULL;
    } else if (can_recurse && child_buf.len > 0 &&
               (ans_ok < 0 || child_buf.len < ans_buf.len + LAYER_HDR)) {
        halt = HALT_RECURSE; child_payload = &child_buf;
    } else if (ans_ok == 0) {
        halt = HALT_ANS; child_payload = &ans_buf;
    } else {
        halt = HALT_ANS; child_payload = &ans_buf;
    }

    /* 9. Emit wire format. */
    uint8_t hdr[LAYER_HDR];
    hdr[0] = (uint8_t)best_bit;
    hdr[1] = (uint8_t)flag_mode;
    uint64_t fl_len = (uint64_t)flag_block.len;
    hdr[2]=(uint8_t)(fl_len>>56); hdr[3]=(uint8_t)(fl_len>>48);
    hdr[4]=(uint8_t)(fl_len>>40); hdr[5]=(uint8_t)(fl_len>>32);
    hdr[6]=(uint8_t)(fl_len>>24); hdr[7]=(uint8_t)(fl_len>>16);
    hdr[8]=(uint8_t)(fl_len>> 8); hdr[9]=(uint8_t)fl_len;
    hdr[10] = halt;
    uint64_t clen = child_payload ? (uint64_t)child_payload->len : 0;
    hdr[11]=(uint8_t)(clen>>56); hdr[12]=(uint8_t)(clen>>48);
    hdr[13]=(uint8_t)(clen>>40); hdr[14]=(uint8_t)(clen>>32);
    hdr[15]=(uint8_t)(clen>>24); hdr[16]=(uint8_t)(clen>>16);
    hdr[17]=(uint8_t)(clen>> 8); hdr[18]=(uint8_t)clen;

    int ret = 0;
    if (buf_append(out, hdr, LAYER_HDR) < 0) ret = -1;
    if (ret == 0 && buf_append(out, flag_block.data, flag_block.len) < 0) ret = -1;
    if (ret == 0 && child_payload && child_payload->len > 0)
        if (buf_append(out, child_payload->data, child_payload->len) < 0)
            ret = -1;

    /* aligned and syms freed early above; buf_free on zeroed Buf is a no-op */
    buf_free(&flag_block); buf_free(&ans_buf); buf_free(&child_buf);
    return ret;
}

/* ── recursive_encode_stream ────────────────────────────────────────── */

int recursive_encode_stream(const char *src_path, size_t n,
                            Buf *out,
                            int *n_layers_out, int *flag_mode_out)
{
    int outer_flag_mode = 0;
    int ret = encode_layer_stream(src_path, n, &outer_flag_mode, out);
    if (ret < 0) return -1;

    /* Count layers by walking the halt chain (identical to recursive_encode). */
    int layers = 0;
    size_t pos = 0;
    while (pos + LAYER_HDR <= out->len) {
        layers++;
        uint64_t fl_len =
            ((uint64_t)out->data[pos+2]<<56)|((uint64_t)out->data[pos+3]<<48)|
            ((uint64_t)out->data[pos+4]<<40)|((uint64_t)out->data[pos+5]<<32)|
            ((uint64_t)out->data[pos+6]<<24)|((uint64_t)out->data[pos+7]<<16)|
            ((uint64_t)out->data[pos+8]<< 8)|(uint64_t)out->data[pos+9];
        uint8_t halt = out->data[pos+10];
        uint64_t child_len =
            ((uint64_t)out->data[pos+11]<<56)|((uint64_t)out->data[pos+12]<<48)|
            ((uint64_t)out->data[pos+13]<<40)|((uint64_t)out->data[pos+14]<<32)|
            ((uint64_t)out->data[pos+15]<<24)|((uint64_t)out->data[pos+16]<<16)|
            ((uint64_t)out->data[pos+17]<< 8)|(uint64_t)out->data[pos+18];
        if (halt != HALT_RECURSE) break;
        pos += LAYER_HDR + fl_len + child_len;
    }

    if (n_layers_out)  *n_layers_out  = layers;
    if (flag_mode_out) *flag_mode_out = outer_flag_mode;
    return 0;
}

/* ── decode_layer ────────────────────────────────────────────────────── */

static int decode_layer(const uint8_t *payload, size_t payload_len,
                        size_t pos, uint64_t orig_n, Buf *out)
{
    if (pos + LAYER_HDR > payload_len) return -1;

    int      bit_stripped = payload[pos];
    int      flag_mode    = payload[pos+1];
    uint64_t fl_len =
        ((uint64_t)payload[pos+2]<<56)|((uint64_t)payload[pos+3]<<48)|
        ((uint64_t)payload[pos+4]<<40)|((uint64_t)payload[pos+5]<<32)|
        ((uint64_t)payload[pos+6]<<24)|((uint64_t)payload[pos+7]<<16)|
        ((uint64_t)payload[pos+8]<< 8)|(uint64_t)payload[pos+9];
    uint8_t  halt = payload[pos+10];
    uint64_t child_len =
        ((uint64_t)payload[pos+11]<<56)|((uint64_t)payload[pos+12]<<48)|
        ((uint64_t)payload[pos+13]<<40)|((uint64_t)payload[pos+14]<<32)|
        ((uint64_t)payload[pos+15]<<24)|((uint64_t)payload[pos+16]<<16)|
        ((uint64_t)payload[pos+17]<< 8)|(uint64_t)payload[pos+18];

    pos += LAYER_HDR;

    if (pos + fl_len > payload_len) return -1;
    const uint8_t *flag_raw  = payload + pos;  pos += fl_len;

    if (pos + child_len > payload_len) return -1;
    const uint8_t *child_data = payload + pos;  pos += child_len;

    /* Decode flags into bitset. */
    size_t   flag_bs_len = ((size_t)orig_n + 7) / 8;
    uint8_t *flag_bs = calloc(1, flag_bs_len);
    if (!flag_bs) return -1;
    if (decode_flags(flag_raw, fl_len, orig_n, flag_mode, flag_bs, flag_bs_len) < 0) {
        free(flag_bs); return -1;
    }

    /* Reconstruct aligned data. */
    Buf aligned; buf_init(&aligned);

    if (halt == HALT_ZERO || halt == HALT_ONE) {
        uint8_t fill = (halt == HALT_ONE) ? 0x7F : 0x00;
        if (buf_reserve_exact(&aligned, (size_t)orig_n) < 0) {
            free(flag_bs); buf_free(&aligned); return -1;
        }
        memset(aligned.data, fill, (size_t)orig_n);
        aligned.len = (size_t)orig_n;
    } else if (halt == HALT_ANS) {
        Buf syms; buf_init(&syms);
        if (ans_stride_decode(child_data, child_len, (size_t)orig_n, &syms) < 0) {
            free(flag_bs); buf_free(&aligned); buf_free(&syms); return -1;
        }
        if (unmap_from_128(syms.data, syms.len, bit_stripped, &aligned) < 0) {
            free(flag_bs); buf_free(&aligned); buf_free(&syms); return -1;
        }
        buf_free(&syms);
    } else { /* HALT_RECURSE */
        int np = decode_layer(child_data, child_len, 0, orig_n, &aligned);
        if (np < 0) { free(flag_bs); buf_free(&aligned); return -1; }
    }

    /* Reconstruct this layer's output. */
    if (buf_reserve_exact(out, out->len + (size_t)orig_n) < 0) {
        free(flag_bs); buf_free(&aligned); return -1;
    }
    size_t prev_len = out->len;
    out->len += (size_t)orig_n;
    reconstruct(aligned.data, (size_t)orig_n, flag_bs, bit_stripped,
                out->data + prev_len);

    buf_free(&aligned);
    free(flag_bs);
    return (int)pos;
}

/* ── recursive_decode ───────────────────────────────────────────────── */

int recursive_decode(const uint8_t *payload, size_t payload_len,
                     uint64_t orig_size, Buf *out_data)
{
    int np = decode_layer(payload, payload_len, 0, orig_size, out_data);
    return (np < 0) ? -1 : 0;
}

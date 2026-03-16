#include "ans.h"
#include "aim3.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * ANS constants (mirror Python)
 * ANS_M = 16384, ANS_L = 16384, ANS_B = 256
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Frequency table types ──────────────────────────────────────────────── */

typedef struct {
    uint16_t freq[128];
    uint32_t cum[129];      /* cum[i+1] = cum[i] + freq[i], cum[128] = ANS_M */
} AnsTable;

/* Slot table for decode: slots[s] = symbol for cumulative slot s. */
typedef uint8_t SlotTable[ANS_M];

/* ── scale_freqs ────────────────────────────────────────────────────────── */

/* raw_counts[0..127]: occurrence counts.  n_obs: total count. */
static void scale_freqs(const uint32_t *raw_counts, uint64_t n_obs, AnsTable *t)
{
    memset(t->freq, 0, sizeof(t->freq));

    /* Proportional scaling with min-1 for observed symbols. */
    int32_t delta = ANS_M;
    for (int s = 0; s < 128; s++) {
        if (raw_counts[s] == 0) continue;
        uint32_t f = (uint32_t)((uint64_t)raw_counts[s] * ANS_M / n_obs);
        if (f == 0) f = 1;
        t->freq[s] = (uint16_t)f;
        delta -= (int32_t)f;
    }

    /* Distribute residual delta to most-frequent symbols (Python order). */
    /* Collect observed symbols sorted by raw count descending. */
    int order[128]; int n_sym = 0;
    for (int s = 0; s < 128; s++) if (raw_counts[s] > 0) order[n_sym++] = s;
    /* Insertion sort by raw_counts descending (n_sym ≤ 128, cheap). */
    for (int i = 1; i < n_sym; i++) {
        int key = order[i]; int j = i - 1;
        while (j >= 0 && raw_counts[order[j]] < raw_counts[key]) {
            order[j+1] = order[j]; j--;
        }
        order[j+1] = key;
    }

    int i = 0, iters = 0;
    while (delta != 0 && n_sym > 0) {
        int s = order[i % n_sym];
        if (delta > 0) { t->freq[s]++; delta--; }
        else { if (t->freq[s] > 1) { t->freq[s]--; delta++; } }
        i++; iters++;
        if (iters > ANS_M) break;
    }

    /* Build cumulative table. */
    t->cum[0] = 0;
    for (int s = 0; s < 128; s++) t->cum[s+1] = t->cum[s] + t->freq[s];
}

/* ── make_slots ─────────────────────────────────────────────────────────── */

static void make_slots(const AnsTable *t, SlotTable slots)
{
    for (int s = 0; s < 128; s++)
        for (uint32_t j = t->cum[s]; j < t->cum[s+1]; j++)
            slots[j] = (uint8_t)s;
}

/* ── pack / unpack full table ───────────────────────────────────────────── */
/* Format: uint16 BE n_nonzero, then n_nonzero × (uint8 sym, uint16 BE freq). */

static int pack_full_table(const AnsTable *t, Buf *out)
{
    /* Count non-zero entries. */
    uint16_t nz = 0;
    for (int s = 0; s < 128; s++) if (t->freq[s]) nz++;
    if (buf_put_u16be(out, nz) < 0) return -1;
    for (int s = 0; s < 128; s++) {
        if (!t->freq[s]) continue;
        if (buf_push(out, (uint8_t)s) < 0) return -1;
        if (buf_put_u16be(out, t->freq[s]) < 0) return -1;
    }
    return 0;
}

/* Returns bytes consumed on success, -1 on error. Fills t and builds cum[]. */
static int unpack_full_table(const uint8_t *payload, size_t plen,
                             size_t pos, AnsTable *t)
{
    if (pos + 2 > plen) return -1;
    uint16_t n = ((uint16_t)payload[pos] << 8) | payload[pos+1]; pos += 2;
    memset(t->freq, 0, sizeof(t->freq));
    for (uint16_t i = 0; i < n; i++) {
        if (pos + 3 > plen) return -1;
        uint8_t  s = payload[pos]; pos++;
        uint16_t f = ((uint16_t)payload[pos] << 8) | payload[pos+1]; pos += 2;
        if (s >= 128) return -1;
        t->freq[s] = f;
    }
    t->cum[0] = 0;
    for (int s = 0; s < 128; s++) t->cum[s+1] = t->cum[s] + t->freq[s];
    return (int)pos;
}

/* ── ANS encode/decode single symbol ────────────────────────────────────── */

static uint32_t encode_sym(uint32_t x, uint8_t s, const AnsTable *t, Buf *out_bytes)
{
    uint32_t f = t->freq[s];
    /* Normalise: emit low bytes until x is in the valid range. */
    uint32_t limit = ((uint32_t)ANS_L / (uint32_t)ANS_M) * (uint32_t)ANS_B * f;
    while (x >= limit) {
        buf_push(out_bytes, (uint8_t)(x & 0xFF));
        x >>= 8;
    }
    return (x / f) * (uint32_t)ANS_M + t->cum[s] + (x % f);
}

static uint32_t decode_sym(uint32_t x, const SlotTable slots, const AnsTable *t,
                           const uint8_t *rev_buf, size_t rev_len,
                           size_t *rpos_inout, uint8_t *sym_out)
{
    uint32_t slot = x % (uint32_t)ANS_M;
    uint8_t  s    = slots[slot];
    x = (uint32_t)t->freq[s] * (x >> ANS_M_BITS) + slot - t->cum[s];
    /* Refill: read bytes from reversed stream. */
    while (x < (uint32_t)ANS_L && *rpos_inout < rev_len) {
        x = (x << 8) | rev_buf[(*rpos_inout)++];
    }
    *sym_out = s;
    return x;
}

/* ════════════════════════════════════════════════════════════════════════════
 * ANS Order-0
 * Format: full_table || init_state (uint32 BE) || reversed_byte_stream
 * ════════════════════════════════════════════════════════════════════════════ */

int ans0_encode(const uint8_t *syms, size_t n, Buf *out)
{
    if (n == 0) return 0;

    /* Count frequencies. */
    uint32_t raw[128] = {0};
    for (size_t i = 0; i < n; i++) raw[syms[i]]++;

    AnsTable t; scale_freqs(raw, (uint64_t)n, &t);

    /* Encode in reverse. */
    Buf byte_out; buf_init(&byte_out);
    uint32_t x = ANS_L;
    for (size_t i = n; i-- > 0; )
        x = encode_sym(x, syms[i], &t, &byte_out);

    /* Pack: table + state + bytes (reversed). */
    if (pack_full_table(&t, out) < 0) { buf_free(&byte_out); return -1; }
    if (buf_put_u32be(out, x)    < 0) { buf_free(&byte_out); return -1; }
    /* Do NOT reverse: decoder reverses payload[pos:] to get decode order. */
    int ret = buf_append(out, byte_out.data, byte_out.len);
    buf_free(&byte_out);
    return ret;
}

int ans0_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out)
{
    if (n == 0) return 0;
    AnsTable t;
    int pos = unpack_full_table(payload, plen, 0, &t);
    if (pos < 0 || (size_t)pos + 4 > plen) return -1;

    uint32_t x = ((uint32_t)payload[pos]<<24)|((uint32_t)payload[pos+1]<<16)
               | ((uint32_t)payload[pos+2]<< 8)| (uint32_t)payload[pos+3];
    pos += 4;

    SlotTable slots; make_slots(&t, slots);

    /* Reversed byte stream. */
    size_t byte_len = plen - (size_t)pos;
    const uint8_t *byte_data = payload + pos;
    /* Build reversed view. */
    uint8_t *rev = malloc(byte_len);
    if (!rev && byte_len) return -1;
    for (size_t i = 0; i < byte_len; i++)
        rev[i] = byte_data[byte_len - 1 - i];
    size_t rpos = 0;

    if (buf_reserve(out, out->len + n) < 0) { free(rev); return -1; }
    for (size_t i = 0; i < n; i++) {
        uint8_t s;
        x = decode_sym(x, slots, &t, rev, byte_len, &rpos, &s);
        out->data[out->len++] = s;
    }
    free(rev);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * O1 context-table helpers
 * ════════════════════════════════════════════════════════════════════════════ */

/* One context entry (may be NULL if context doesn't meet threshold). */
typedef struct {
    AnsTable  tbl;
    SlotTable slots;
    int       valid;
} O1Ctx;

/* Build O1 tables. caller allocates ctx_tables[128]. */
static void build_o1_tables(const uint8_t *syms, size_t n,
                             AnsTable *gf,
                             O1Ctx *ctx_tables)
{
    /* Global table. */
    uint32_t raw_g[128] = {0};
    for (size_t i = 0; i < n; i++) raw_g[syms[i]]++;
    scale_freqs(raw_g, (uint64_t)n, gf);

    /* Per-context counts. */
    uint32_t ctx_raw[128][128];
    uint32_t ctx_n[128];
    memset(ctx_raw, 0, sizeof(ctx_raw));
    memset(ctx_n,   0, sizeof(ctx_n));

    for (size_t i = 1; i < n; i++) {
        uint8_t ctx = syms[i-1];
        uint8_t sym = syms[i];
        ctx_raw[ctx][sym]++;
        ctx_n[ctx]++;
    }

    for (int ctx = 0; ctx < 128; ctx++) {
        ctx_tables[ctx].valid = 0;
        if (ctx_n[ctx] < MIN_CTX_O1) continue;
        scale_freqs(ctx_raw[ctx], ctx_n[ctx], &ctx_tables[ctx].tbl);
        make_slots(&ctx_tables[ctx].tbl, ctx_tables[ctx].slots);
        ctx_tables[ctx].valid = 1;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ANS Order-1
 * Format:
 *   global_table || 16-byte ctx bitmask ||
 *   per-ctx tables (for each valid ctx in order 0..127) ||
 *   first_sym (uint8) || state (uint32 BE) || reversed_bytes
 * ════════════════════════════════════════════════════════════════════════════ */

int ans1_encode(const uint8_t *syms, size_t n, Buf *out)
{
    if (n == 0) return 0;

    AnsTable gf;
    O1Ctx    ctx_tables[128];
    build_o1_tables(syms, n, &gf, ctx_tables);

    /* Build bitmask. */
    uint8_t mask[16] = {0};
    for (int c = 0; c < 128; c++)
        if (ctx_tables[c].valid) mask[c >> 3] |= (uint8_t)(1u << (c & 7));

    /* Encode in reverse (from index n-1 down to 1). */
    Buf byte_out; buf_init(&byte_out);
    uint32_t x = ANS_L;
    for (size_t i = n - 1; i >= 1; i--) {
        uint8_t  s   = syms[i];
        uint8_t  ctx = syms[i - 1];
        const AnsTable *t = ctx_tables[ctx].valid ? &ctx_tables[ctx].tbl : &gf;
        x = encode_sym(x, s, t, &byte_out);
    }

    /* Emit: global_table, mask, per-ctx tables, first_sym, state, bytes. */
    if (pack_full_table(&gf, out) < 0) { buf_free(&byte_out); return -1; }
    if (buf_append(out, mask, 16) < 0) { buf_free(&byte_out); return -1; }
    for (int c = 0; c < 128; c++) {
        if (!ctx_tables[c].valid) continue;
        if (pack_full_table(&ctx_tables[c].tbl, out) < 0) {
            buf_free(&byte_out); return -1;
        }
    }
    if (buf_push(out, syms[0])   < 0) { buf_free(&byte_out); return -1; }
    if (buf_put_u32be(out, x)    < 0) { buf_free(&byte_out); return -1; }
    int ret = buf_append(out, byte_out.data, byte_out.len);
    buf_free(&byte_out);
    return ret;
}

int ans1_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out)
{
    if (n == 0) return 0;

    AnsTable gf; SlotTable gslots;
    int pos = unpack_full_table(payload, plen, 0, &gf);
    if (pos < 0) return -1;
    make_slots(&gf, gslots);

    if ((size_t)pos + 16 > plen) return -1;
    const uint8_t *mask = payload + pos; pos += 16;

    /* Unpack per-context tables. */
    O1Ctx ctx_tables[128];
    memset(ctx_tables, 0, sizeof(ctx_tables));
    for (int c = 0; c < 128; c++) {
        if (!((mask[c >> 3] >> (c & 7)) & 1)) continue;
        int np = unpack_full_table(payload, plen, (size_t)pos, &ctx_tables[c].tbl);
        if (np < 0) return -1;
        pos = np;
        make_slots(&ctx_tables[c].tbl, ctx_tables[c].slots);
        ctx_tables[c].valid = 1;
    }

    if ((size_t)pos + 5 > plen) return -1;
    uint8_t  init = payload[pos]; pos++;
    uint32_t x = ((uint32_t)payload[pos]<<24)|((uint32_t)payload[pos+1]<<16)
               | ((uint32_t)payload[pos+2]<< 8)| (uint32_t)payload[pos+3];
    pos += 4;

    size_t byte_len = plen - (size_t)pos;
    const uint8_t *byte_data = payload + pos;
    uint8_t *rev = malloc(byte_len);
    if (!rev && byte_len) return -1;
    for (size_t i = 0; i < byte_len; i++)
        rev[i] = byte_data[byte_len - 1 - i];
    size_t rpos = 0;

    if (buf_reserve(out, out->len + n) < 0) { free(rev); return -1; }
    out->data[out->len++] = init;
    uint8_t ctx = init;
    for (size_t i = 1; i < n; i++) {
        const AnsTable *t;
        const SlotTable *sl;
        if (ctx_tables[ctx].valid) { t = &ctx_tables[ctx].tbl; sl = &ctx_tables[ctx].slots; }
        else                       { t = &gf;                   sl = (const SlotTable *)&gslots; }
        uint8_t s;
        x = decode_sym(x, *sl, t, rev, byte_len, &rpos, &s);
        out->data[out->len++] = s;
        ctx = s;
    }
    free(rev);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Differential table helpers (O2d)
 * ════════════════════════════════════════════════════════════════════════════ */

/* Pack a differential table vs parent_freq.
 * Format: uint16 BE n_overrides, then n × (uint8 sym, uint16 BE freq).
 * Only symbols whose freq differs from parent by > DELTA_THRESHOLD are stored.
 */
static int pack_diff_table(const AnsTable *child, const uint16_t *parent_freq, Buf *out)
{
    /* Collect overrides: every symbol where child freq differs from parent. */
    int ov_sym[128]; uint16_t ov_freq[128]; int n_ov = 0;
    for (int s = 0; s < 128; s++) {
        if (child->freq[s] != parent_freq[s]) {
            ov_sym[n_ov] = s; ov_freq[n_ov] = child->freq[s]; n_ov++;
        }
    }
    if (buf_put_u16be(out, (uint16_t)n_ov) < 0) return -1;
    for (int i = 0; i < n_ov; i++) {
        if (buf_push(out, (uint8_t)ov_sym[i]) < 0) return -1;
        if (buf_put_u16be(out, ov_freq[i]) < 0) return -1;
    }
    return 0;
}

/* Unpack a differential table, adjusting for the residual delta.
 * Returns new pos on success, -1 on error. */
static int unpack_diff_table(const uint8_t *payload, size_t plen,
                             size_t pos, const uint16_t *parent_freq,
                             AnsTable *out_t)
{
    if (pos + 2 > plen) return -1;
    uint16_t n = ((uint16_t)payload[pos] << 8) | payload[pos+1]; pos += 2;

    /* Start from parent frequencies. */
    for (int s = 0; s < 128; s++) out_t->freq[s] = parent_freq[s];

    int stored[128]; int n_stored = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (pos + 3 > plen) return -1;
        uint8_t  s = payload[pos]; pos++;
        uint16_t f = ((uint16_t)payload[pos] << 8) | payload[pos+1]; pos += 2;
        if (s >= 128) return -1;
        out_t->freq[s] = f;
        stored[n_stored++] = s;
    }

    /* Compute residual delta and adjust highest-frequency stored symbol. */
    int32_t sum = 0;
    for (int s = 0; s < 128; s++) sum += out_t->freq[s];
    int32_t delta = ANS_M - sum;
    if (delta != 0) {
        /* Find the stored symbol with highest frequency. */
        int adj = -1;
        if (n_stored > 0) {
            adj = stored[0];
            for (int i = 1; i < n_stored; i++)
                if (out_t->freq[stored[i]] > out_t->freq[adj]) adj = stored[i];
        } else {
            /* No stored symbols: find global max. */
            adj = 0;
            for (int s = 1; s < 128; s++)
                if (out_t->freq[s] > out_t->freq[adj]) adj = s;
        }
        if (adj >= 0 && (int32_t)out_t->freq[adj] + delta >= 1) {
            out_t->freq[adj] += (uint16_t)delta;
        } else if (adj >= 0) {
            /* Distribute remainder across symbols by descending freq. */
            uint8_t processed[128] = {0};
            int32_t remaining = delta;
            for (int iter = 0; iter < 128 && remaining != 0; iter++) {
                int best = -1;
                for (int s = 0; s < 128; s++) {
                    if (processed[s]) continue;
                    if (out_t->freq[s] > 0 && (best < 0 || out_t->freq[s] > out_t->freq[best]))
                        best = s;
                }
                if (best < 0) break;
                int32_t take = remaining;
                if (take < 0 && -take > (int32_t)(out_t->freq[best] - 1))
                    take = -((int32_t)(out_t->freq[best] - 1));
                out_t->freq[best] += (uint16_t)take;
                remaining -= take;
                processed[best] = 1;
            }
        }
    }

    out_t->cum[0] = 0;
    for (int s = 0; s < 128; s++) out_t->cum[s+1] = out_t->cum[s] + out_t->freq[s];
    return (int)pos;
}

/* ── KL gain ───────────────────────────────────────────────────────────── */

static double kl_gain_bytes(const uint32_t *child_raw, uint32_t n_obs,
                             const uint16_t *parent_freq)
{
    double gain = 0.0;
    for (int s = 0; s < 128; s++) {
        if (!child_raw[s]) continue;
        double pp = (double)parent_freq[s] / ANS_M;
        if (pp <= 0.0) pp = 1.0 / ANS_M;
        double cp = (double)child_raw[s] / n_obs;
        gain += (double)child_raw[s] * log2(cp / pp);
    }
    return gain / 8.0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * ANS Order-2 differential
 *
 * O2 context key: syms[i-2]*128 + syms[i-1]  (range 0..16383)
 *
 * Format:
 *   global_table ||
 *   o1_mask (16 bytes) || o1_tables ||
 *   o2_mask (2048 bytes) || o2_tables ||
 *   sym0 (uint8) || sym1 (uint8) ||
 *   state (uint32 BE) || reversed_bytes
 * ════════════════════════════════════════════════════════════════════════════ */

/* Per-O2 context entry stored on heap (only qualifying contexts). */
typedef struct {
    AnsTable  tbl;
    SlotTable slots;
    uint8_t   is_diff; /* 1 = stored as diff blob, 0 = full table */
    /* Serialised blob (diff or full) for the encoder output pass. */
    uint8_t  *blob;
    size_t    blob_len;
} O2Ctx;

int ans2_diff_encode(const uint8_t *syms, size_t n, Buf *out)
{
    if (n < 2) return 0;

    /* ── Global and O1 tables ── */
    AnsTable gf;
    O1Ctx    o1[128];
    build_o1_tables(syms, n, &gf, o1);

    /* ── O2 context counts ── */
    /* key = syms[i-2]*128 + syms[i-1], 16384 possible keys. */
    uint32_t (*o2_raw)[128] = calloc(16384, sizeof(*o2_raw));
    uint32_t  *o2_n         = calloc(16384, sizeof(uint32_t));
    if (!o2_raw || !o2_n) { free(o2_raw); free(o2_n); return -1; }

    for (size_t i = 2; i < n; i++) {
        int key = (int)syms[i-2] * 128 + (int)syms[i-1];
        o2_raw[key][syms[i]]++;
        o2_n[key]++;
    }

    /* ── Build qualifying O2 entries ── */
    O2Ctx *o2_ctx = calloc(16384, sizeof(O2Ctx));
    if (!o2_ctx) { free(o2_raw); free(o2_n); return -1; }

    uint8_t o2_mask[2048]; memset(o2_mask, 0, sizeof(o2_mask));

    for (int key = 0; key < 16384; key++) {
        if (o2_n[key] < MIN_CTX_O2) continue;
        int prev1 = key & 0x7F;
        const uint16_t *pf = o1[prev1].valid ? o1[prev1].tbl.freq : gf.freq;

        if (kl_gain_bytes(o2_raw[key], o2_n[key], pf) < MIN_CODING_GAIN) continue;

        /* Build child freq table. */
        AnsTable cf; scale_freqs(o2_raw[key], o2_n[key], &cf);

        /* Build diff blob. */
        Buf diff_b; buf_init(&diff_b);
        if (pack_diff_table(&cf, pf, &diff_b) < 0) {
            buf_free(&diff_b); continue;
        }
        /* Build full blob. */
        Buf full_b; buf_init(&full_b);
        if (pack_full_table(&cf, &full_b) < 0) {
            buf_free(&diff_b); buf_free(&full_b); continue;
        }

        /* Choose smaller blob. */
        uint8_t *blob; size_t blob_len; int is_diff;
        if (diff_b.len <= full_b.len) {
            blob = diff_b.data; blob_len = diff_b.len; is_diff = 1;
            buf_free(&full_b);
            diff_b.data = NULL; /* don't double-free */
        } else {
            blob = full_b.data; blob_len = full_b.len; is_diff = 0;
            buf_free(&diff_b);
            full_b.data = NULL;
        }

        o2_ctx[key].tbl      = cf;
        o2_ctx[key].is_diff  = (uint8_t)is_diff;
        o2_ctx[key].blob     = blob;
        o2_ctx[key].blob_len = blob_len;
        make_slots(&cf, o2_ctx[key].slots);
        o2_mask[key >> 3] |= (uint8_t)(1u << (key & 7));
    }
    free(o2_raw); free(o2_n);

    /* ── Build O1 mask ── */
    uint8_t o1_mask[16] = {0};
    for (int c = 0; c < 128; c++)
        if (o1[c].valid) o1_mask[c >> 3] |= (uint8_t)(1u << (c & 7));

    /* ── Encode in reverse (from n-1 down to 2) ── */
    Buf byte_out; buf_init(&byte_out);
    uint32_t x = ANS_L;

    for (size_t i = n - 1; i >= 2; i--) {
        uint8_t s   = syms[i];
        int     key = (int)syms[i-2] * 128 + (int)syms[i-1];
        int     p1  = (int)syms[i-1];
        const AnsTable *t;
        if ((o2_mask[key>>3] >> (key&7)) & 1) t = &o2_ctx[key].tbl;
        else if (o1[p1].valid)                 t = &o1[p1].tbl;
        else                                   t = &gf;
        x = encode_sym(x, s, t, &byte_out);
    }

    /* ── Emit header sections ── */
    int ret = 0;
    if (pack_full_table(&gf, out) < 0) { ret = -1; goto cleanup; }
    if (buf_append(out, o1_mask, 16) < 0) { ret = -1; goto cleanup; }
    for (int c = 0; c < 128; c++) {
        if (!o1[c].valid) continue;
        if (pack_full_table(&o1[c].tbl, out) < 0) { ret = -1; goto cleanup; }
    }
    if (buf_append(out, o2_mask, 2048) < 0) { ret = -1; goto cleanup; }
    for (int key = 0; key < 16384; key++) {
        if (!((o2_mask[key>>3] >> (key&7)) & 1)) continue;
        uint8_t tag = o2_ctx[key].is_diff ? 0x01 : 0x00;
        if (buf_push(out, tag) < 0) { ret = -1; goto cleanup; }
        if (buf_append(out, o2_ctx[key].blob, o2_ctx[key].blob_len) < 0) {
            ret = -1; goto cleanup;
        }
    }
    if (buf_push(out, syms[0]) < 0) { ret = -1; goto cleanup; }
    if (buf_push(out, syms[1]) < 0) { ret = -1; goto cleanup; }
    if (buf_put_u32be(out, x)  < 0) { ret = -1; goto cleanup; }
    if (buf_append(out, byte_out.data, byte_out.len) < 0) { ret = -1; goto cleanup; }

cleanup:
    buf_free(&byte_out);
    for (int key = 0; key < 16384; key++) free(o2_ctx[key].blob);
    free(o2_ctx);
    return ret;
}

int ans2_diff_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out)
{
    if (n < 2) {
        if (n > 0 && plen > 0) {
            if (buf_push(out, payload[0]) < 0) return -1;
        }
        return 0;
    }

    size_t pos = 0;

    /* Global table. */
    AnsTable gf; SlotTable gslots;
    int np = unpack_full_table(payload, plen, pos, &gf);
    if (np < 0) return -1;
    pos = (size_t)np;
    make_slots(&gf, gslots);

    /* O1 mask + tables. */
    if (pos + 16 > plen) return -1;
    const uint8_t *o1m = payload + pos; pos += 16;

    O1Ctx o1[128]; memset(o1, 0, sizeof(o1));
    for (int c = 0; c < 128; c++) {
        if (!((o1m[c>>3] >> (c&7)) & 1)) continue;
        np = unpack_full_table(payload, plen, pos, &o1[c].tbl);
        if (np < 0) return -1;
        pos = (size_t)np;
        make_slots(&o1[c].tbl, o1[c].slots);
        o1[c].valid = 1;
    }

    /* O2 mask + tables. */
    if (pos + 2048 > plen) return -1;
    const uint8_t *o2m = payload + pos; pos += 2048;

    /* Decode O2 tables on demand; store decoded AnsTable + slots.
     * o2_slots is lazily allocated (one SlotTable per valid key) so that
     * files with sparse O2 usage don't pay the full 256 MB upfront cost. */
    AnsTable   *o2_tbls  = calloc(16384, sizeof(AnsTable));
    SlotTable **o2_slots = calloc(16384, sizeof(SlotTable *));  /* NULL = unset */
    uint8_t    *o2_valid = calloc(16384, 1);
    if (!o2_tbls || !o2_slots || !o2_valid) {
        free(o2_tbls); free(o2_slots); free(o2_valid); return -1;
    }

    for (int key = 0; key < 16384; key++) {
        if (!((o2m[key>>3] >> (key&7)) & 1)) continue;
        int p1 = key & 0x7F;
        const uint16_t *pf = o1[p1].valid ? o1[p1].tbl.freq : gf.freq;
        /* Read explicit format tag (0x01 = diff, 0x00 = full). */
        if (pos + 1 > plen) {
            free(o2_tbls);
            for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
            free(o2_slots); free(o2_valid); return -1;
        }
        uint8_t tag = payload[pos++];
        if (tag == 0x01) {
            np = unpack_diff_table(payload, plen, pos, pf, &o2_tbls[key]);
        } else {
            np = unpack_full_table(payload, plen, pos, &o2_tbls[key]);
        }
        if (np < 0) {
            free(o2_tbls);
            for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
            free(o2_slots); free(o2_valid); return -1;
        }
        pos = (size_t)np;
        o2_slots[key] = malloc(sizeof(SlotTable));
        if (!o2_slots[key]) {
            free(o2_tbls);
            for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
            free(o2_slots); free(o2_valid); return -1;
        }
        make_slots(&o2_tbls[key], *o2_slots[key]);
        o2_valid[key] = 1;
    }

    /* First two symbols + state. */
    if (pos + 6 > plen) {
        free(o2_tbls);
        for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
        free(o2_slots); free(o2_valid); return -1;
    }
    uint8_t  s0 = payload[pos]; uint8_t s1 = payload[pos+1]; pos += 2;
    uint32_t x  = ((uint32_t)payload[pos]<<24)|((uint32_t)payload[pos+1]<<16)
                | ((uint32_t)payload[pos+2]<< 8)| (uint32_t)payload[pos+3];
    pos += 4;

    /* Build reversed byte stream. */
    size_t byte_len = plen - pos;
    uint8_t *rev = malloc(byte_len);
    if (!rev && byte_len) {
        free(o2_tbls);
        for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
        free(o2_slots); free(o2_valid); return -1;
    }
    for (size_t i = 0; i < byte_len; i++) rev[i] = payload[plen - 1 - i];
    size_t rpos = 0;

    if (buf_reserve(out, out->len + n) < 0) {
        free(rev); free(o2_tbls);
        for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
        free(o2_slots); free(o2_valid); return -1;
    }
    out->data[out->len++] = s0;
    out->data[out->len++] = s1;

    for (size_t i = 2; i < n; i++) {
        int     key = (int)out->data[out->len - 2] * 128 + (int)out->data[out->len - 1];
        int     p1  = (int)out->data[out->len - 1];
        const AnsTable *t; const SlotTable *sl;
        if (o2_valid[key]) { t = &o2_tbls[key];  sl = o2_slots[key]; }
        else if (o1[p1].valid) { t = &o1[p1].tbl; sl = (const SlotTable *)o1[p1].slots; }
        else { t = &gf; sl = (const SlotTable *)&gslots; }
        uint8_t s;
        x = decode_sym(x, *sl, t, rev, byte_len, &rpos, &s);
        out->data[out->len++] = s;
    }

    free(rev); free(o2_tbls);
    for (int k2 = 0; k2 < 16384; k2++) free(o2_slots[k2]);
    free(o2_slots); free(o2_valid);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * ANS-Stride (backend 5)
 *
 * Order-1 ANS with stride k: context for symbol i is syms[i-k].
 * k is selected by minimising H(X_i | X_{i-k}) over candidate strides.
 * ════════════════════════════════════════════════════════════════════════════ */

/* Compute H(X_i | X_{i-k}) in nats, sampled over the first `cap` symbols. */
static double cond_entropy_stride(const uint8_t *syms, size_t n,
                                  size_t cap, int k)
{
    if (n > cap) n = cap;
    /* ctx_count[c] = total symbols with context c. */
    /* pair[c][s]   = count of symbol s given context c. */
    uint32_t *ctx_count = calloc(128, sizeof(uint32_t));
    uint32_t *pair      = calloc(128 * 128, sizeof(uint32_t));
    if (!ctx_count || !pair) { free(ctx_count); free(pair); return 1e30; }

    for (size_t i = (size_t)k; i < n; i++) {
        uint8_t ctx = syms[i - (size_t)k];
        uint8_t sym = syms[i];
        ctx_count[ctx]++;
        pair[ctx * 128 + sym]++;
    }

    double H = 0.0;
    double total = (double)(n > (size_t)k ? n - (size_t)k : 0);
    if (total <= 0) { free(ctx_count); free(pair); return 1e30; }

    for (int c = 0; c < 128; c++) {
        if (!ctx_count[c]) continue;
        double pc = (double)ctx_count[c] / total;
        for (int s = 0; s < 128; s++) {
            uint32_t cnt = pair[c * 128 + s];
            if (!cnt) continue;
            double psc = (double)cnt / (double)ctx_count[c];
            H -= pc * psc * log2(psc);
        }
    }
    free(ctx_count);
    free(pair);
    return H;
}

int ans_select_stride(const uint8_t *syms, size_t n)
{
    static const int candidates[] = {1, 2, 3, 4, 6, 8, 12, 16};
    static const int n_cand = 8;
    size_t cap = n < 1000000 ? n : 1000000;

    double h1 = cond_entropy_stride(syms, n, cap, 1);
    double best_h = h1; int best_k = 1;

    for (int ci = 1; ci < n_cand; ci++) {
        int k = candidates[ci];
        if ((size_t)k >= n) break;
        double h = cond_entropy_stride(syms, n, cap, k);
        if (h < best_h) { best_h = h; best_k = k; }
    }
    /* Only switch if gain ≥ 5% vs k=1. */
    if (h1 > 0.0 && best_h >= h1 * 0.95) return 1;
    return best_k;
}

/* ── ans_stride_encode ──────────────────────────────────────────────────── */

int ans_stride_encode(const uint8_t *syms, size_t n, Buf *out)
{
    if (n == 0) return 0;

    int k = ans_select_stride(syms, n);

    /* Prepend k. */
    if (buf_push(out, (uint8_t)k) < 0) return -1;

    /* Build O1-style tables using context = syms[i-k]. */
    /* Global table over all symbols. */
    uint32_t raw_g[128] = {0};
    for (size_t i = 0; i < n; i++) raw_g[syms[i]]++;
    AnsTable gf; scale_freqs(raw_g, (uint64_t)n, &gf);

    /* Per-context tables with stride k. */
    uint32_t ctx_raw[128][128];
    uint32_t ctx_n[128];
    memset(ctx_raw, 0, sizeof(ctx_raw));
    memset(ctx_n,   0, sizeof(ctx_n));
    for (size_t i = (size_t)k; i < n; i++) {
        uint8_t ctx = syms[i - (size_t)k];
        ctx_raw[ctx][syms[i]]++;
        ctx_n[ctx]++;
    }
    O1Ctx ctx_tables[128];
    for (int c = 0; c < 128; c++) {
        ctx_tables[c].valid = 0;
        if (ctx_n[c] < MIN_CTX_O1) continue;
        scale_freqs(ctx_raw[c], ctx_n[c], &ctx_tables[c].tbl);
        make_slots(&ctx_tables[c].tbl, ctx_tables[c].slots);
        ctx_tables[c].valid = 1;
    }

    /* Build bitmask. */
    uint8_t mask[16] = {0};
    for (int c = 0; c < 128; c++)
        if (ctx_tables[c].valid) mask[c >> 3] |= (uint8_t)(1u << (c & 7));

    /* Encode in reverse (n-1 down to 1), using stride-k context. */
    Buf byte_out; buf_init(&byte_out);
    uint32_t x = ANS_L;
    for (size_t i = n - 1; i >= 1; i--) {
        uint8_t  s   = syms[i];
        uint8_t  ctx = (i >= (size_t)k) ? syms[i - (size_t)k] : 0;
        const AnsTable *t = ctx_tables[ctx].valid ? &ctx_tables[ctx].tbl : &gf;
        x = encode_sym(x, s, t, &byte_out);
    }

    /* Emit global_table, mask, per-ctx tables, first_sym, state, bytes. */
    if (pack_full_table(&gf, out) < 0) { buf_free(&byte_out); return -1; }
    if (buf_append(out, mask, 16) < 0) { buf_free(&byte_out); return -1; }
    for (int c = 0; c < 128; c++) {
        if (!ctx_tables[c].valid) continue;
        if (pack_full_table(&ctx_tables[c].tbl, out) < 0) {
            buf_free(&byte_out); return -1;
        }
    }
    if (buf_push(out, syms[0])  < 0) { buf_free(&byte_out); return -1; }
    if (buf_put_u32be(out, x)   < 0) { buf_free(&byte_out); return -1; }
    int ret = buf_append(out, byte_out.data, byte_out.len);
    buf_free(&byte_out);
    return ret;
}

/* ── ans_stride_decode ──────────────────────────────────────────────────── */

int ans_stride_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out)
{
    if (n == 0) return 0;
    if (plen < 1) return -1;

    int k = (int)payload[0];
    if (k < 1 || k > 64) return -1;   /* sanity: stride must be reasonable */
    payload++; plen--;

    /* Decode identical to ans1_decode but with stride-k context lookup. */
    AnsTable gf; SlotTable gslots;
    int pos = unpack_full_table(payload, plen, 0, &gf);
    if (pos < 0) return -1;
    make_slots(&gf, gslots);

    if ((size_t)pos + 16 > plen) return -1;
    const uint8_t *mask = payload + pos; pos += 16;

    O1Ctx ctx_tables[128];
    memset(ctx_tables, 0, sizeof(ctx_tables));
    for (int c = 0; c < 128; c++) {
        if (!((mask[c >> 3] >> (c & 7)) & 1)) continue;
        int np = unpack_full_table(payload, plen, (size_t)pos, &ctx_tables[c].tbl);
        if (np < 0) return -1;
        pos = np;
        make_slots(&ctx_tables[c].tbl, ctx_tables[c].slots);
        ctx_tables[c].valid = 1;
    }

    if ((size_t)pos + 5 > plen) return -1;
    uint8_t  init = payload[pos]; pos++;
    uint32_t x = ((uint32_t)payload[pos]<<24)|((uint32_t)payload[pos+1]<<16)
               | ((uint32_t)payload[pos+2]<< 8)| (uint32_t)payload[pos+3];
    pos += 4;

    size_t byte_len = plen - (size_t)pos;
    const uint8_t *byte_data = payload + pos;
    uint8_t *rev = malloc(byte_len);
    if (!rev && byte_len) return -1;
    for (size_t i = 0; i < byte_len; i++)
        rev[i] = byte_data[byte_len - 1 - i];
    size_t rpos = 0;

    if (buf_reserve(out, out->len + n) < 0) { free(rev); return -1; }
    out->data[out->len++] = init;

    for (size_t i = 1; i < n; i++) {
        uint8_t ctx = (i >= (size_t)k) ? out->data[out->len - (size_t)k] : 0;
        const AnsTable *t; const SlotTable *sl;
        if (ctx_tables[ctx].valid) { t = &ctx_tables[ctx].tbl; sl = (const SlotTable *)ctx_tables[ctx].slots; }
        else                       { t = &gf;                   sl = (const SlotTable *)&gslots; }
        uint8_t s;
        x = decode_sym(x, *sl, t, rev, byte_len, &rpos, &s);
        out->data[out->len++] = s;
    }
    free(rev);
    return 0;
}

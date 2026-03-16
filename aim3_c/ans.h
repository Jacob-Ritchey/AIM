#ifndef ANS_H
#define ANS_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"

/*
 * rANS backends — Order 0, 1, and 2d (differential).
 * All operate on symbols in [0, 127].
 *
 * Encode: returns 0 on success, -1 on error. Output appended to `out`.
 * Decode: returns 0 on success, -1 on error. Output appended to `out`.
 *         `n` is the expected number of decoded symbols.
 */

int ans0_encode(const uint8_t *syms, size_t n, Buf *out);
int ans0_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out);

int ans1_encode(const uint8_t *syms, size_t n, Buf *out);
int ans1_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out);

int ans2_diff_encode(const uint8_t *syms, size_t n, Buf *out);
int ans2_diff_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out);

/*
 * ANS-Stride backend (BACKEND_ANS_STRIDE = 5).
 *
 * Selects the stride k ∈ {1,2,3,4,6,8,12,16} that minimises H(X_i|X_{i-k}).
 * Falls back to k=1 if the best gain is less than 5% vs stride-1.
 * Context for symbol i is syms[i-k]; out-of-bounds positions use context 0.
 *
 * Wire format:
 *   k (uint8, 1 byte) || ans1_encode output (global table, mask, per-ctx
 *   tables, init_sym, state, reversed bytes) using context = syms[i-k].
 */
int  ans_select_stride(const uint8_t *syms, size_t n);
int  ans_stride_encode(const uint8_t *syms, size_t n, Buf *out);
int  ans_stride_decode(const uint8_t *payload, size_t plen, size_t n, Buf *out);

#endif /* ANS_H */

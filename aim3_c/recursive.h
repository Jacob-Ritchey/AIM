#ifndef RECURSIVE_H
#define RECURSIVE_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"

/*
 * Recursive multi-pass bit-plane extraction encoder/decoder.
 * Container version 0x0A.
 *
 * Algorithm:
 *   1. At each layer, pick the bit-plane with the fewest set bits (sparsest).
 *   2. Extract that bit → flag block (best of gzip/EF/RLE).
 *   3. Remap the residual to [0,127] and try ans_stride_encode on it.
 *   4. Also recurse one more layer.
 *   5. Bottom-up: choose whichever is smaller — ANS-stride terminal or recursive child.
 *   6. Stop when max_layers reached, or when the residual is all-zeros/all-ones.
 *
 * Wire format of the payload (nested layers inside aln_block):
 *
 *   Per layer:
 *     [1]   bit_stripped  uint8   (0-7, which bit was removed)
 *     [1]   flag_mode     uint8   (FLAG_MODE_*)
 *     [4]   flag_len      uint32 BE
 *     [flag_len] flag_block
 *     [1]   halt_code     uint8
 *              0 = HALT_RECURSE: child is next layer
 *              2 = HALT_ZERO:    residual is all-zeros (no child)
 *              3 = HALT_ONE:     residual is all-ones  (no child)
 *              4 = HALT_ANS:     child is ANS-stride terminal payload
 *     [4]   child_len     uint32 BE  (0 for HALT_ZERO/HALT_ONE)
 *     [child_len] child
 *
 * The outer 56-byte header (version 0x0A):
 *   target_bit field (byte [5]) = n_layers actually used (1..8)
 *   flag_mode  field (byte [6]) = flag_mode of outermost layer
 *   backend    field (byte [7]) = BACKEND_ANS_STRIDE (5) always
 *   flag_len   field            = flag block of outermost layer
 *   aln_len    field            = total recursive payload size
 */

#define RECURSIVE_VERSION  0x0A
#define MAX_RECURSIVE_LAYERS 8

#define HALT_RECURSE 0
#define HALT_ZERO    2
#define HALT_ONE     3
#define HALT_ANS     4

/*
 * Encode `data` of length `n` using recursive bit-plane extraction.
 * Returns 0 on success, -1 on error. Output appended to `out`.
 * `out` receives the full payload (flag_block of layer 0 + nested layers).
 * `n_layers_out` is set to the number of layers actually used.
 * `flag_mode_out` is set to the flag mode of the outermost layer.
 */
int recursive_encode(const uint8_t *data, size_t n,
                     Buf *out,
                     int *n_layers_out, int *flag_mode_out);

/*
 * Streaming variant: identical output as recursive_encode but avoids holding
 * an N-byte input buffer.  Depth-0 bit selection and bit_clear are done by
 * streaming reads from `src_path`; deeper layers work on in-memory residuals
 * as usual.
 */
int recursive_encode_stream(const char *src_path, size_t n,
                            Buf *out,
                            int *n_layers_out, int *flag_mode_out);

/*
 * Decode a recursive payload back to the original data.
 * `orig_size` = expected output length.
 * Returns 0 on success, -1 on error. Output appended to `out_data`.
 */
int recursive_decode(const uint8_t *payload, size_t payload_len,
                     uint64_t orig_size, Buf *out_data);

#endif /* RECURSIVE_H */

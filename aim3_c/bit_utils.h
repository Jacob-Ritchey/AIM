#ifndef BIT_UTILS_H
#define BIT_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "aim3.h"

/*
 * Strip `bit` (0-7) from every byte of `data`.
 * aligned_out receives a copy with that bit zeroed.
 * flag_bs must be pre-allocated by caller: calloc(1, (n+7)/8).
 * Bit i is set in flag_bs iff byte i had the target bit set.
 * Returns 0 on success, -1 on OOM.
 */
int bit_clear(const uint8_t *data, size_t n, int bit,
              Buf *aligned_out, uint8_t *flag_bs);

/*
 * Streaming version of bit_clear: reads `path` in 64 KB chunks.
 * flag_bs must be pre-allocated by caller: calloc(1, (n+7)/8).
 * Returns 0 on success, -1 on I/O error or OOM.
 */
int bit_clear_stream(const char *path, size_t n, int bit,
                     Buf *aligned_out, uint8_t *flag_bs);

/*
 * Restore `bit` at every position marked in flag_bs.
 * aligned and result may point to different buffers (result must be pre-sized).
 */
void reconstruct(const uint8_t *aligned, size_t n,
                 const uint8_t *flag_bs, int bit,
                 uint8_t *result);

/*
 * Compact aligned bytes (bit b already zeroed) into [0,127].
 * Formula: out[i] = (v & lo) | ((v >> 1) & (0x7F ^ lo))  where lo = (1<<bit)-1
 * Result written into out_buf (must be pre-allocated to length n, or use buf version).
 */
int remap_to_128(const uint8_t *aligned, size_t n, int bit, Buf *out);

/*
 * Inverse of remap_to_128.
 * Formula: out[i] = (s & lo) | ((s & hi_mask) << 1)  where hi_mask = 0x7F ^ lo
 */
int unmap_from_128(const uint8_t *syms, size_t n, int bit, Buf *out);

#endif /* BIT_UTILS_H */

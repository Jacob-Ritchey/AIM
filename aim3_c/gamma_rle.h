#ifndef GAMMA_RLE_H
#define GAMMA_RLE_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "aim3.h"

/*
 * Encode flag positions as Elias Gamma RLE of the raw flag bitstream.
 *
 * Wire format:
 *   [0]   first_bit  uint8     (0 or 1)
 *   [1-4] n_runs     uint32 BE
 *   [5..] gamma_data bytes     (Elias gamma codes, MSB-first packed)
 *
 * Returns 0 on success, -1 on OOM.
 */
int rle_encode(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n, Buf *out);

/*
 * Decode gamma RLE into a pre-allocated bitset.
 * flag_bs must be calloc'd to (n+7)/8 bytes by caller.
 * Returns 0 on success, -1 on error.
 */
int rle_decode(const uint8_t *data, size_t data_len, uint64_t n,
               uint8_t *flag_bs, size_t flag_bs_len);

#endif /* GAMMA_RLE_H */

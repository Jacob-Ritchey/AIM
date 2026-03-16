#ifndef ELIAS_FANO_H
#define ELIAS_FANO_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "aim3.h"

/*
 * Elias-Fano encoding of a sorted set of flag positions.
 *
 * Wire format (big-endian):
 *   [0-7]   N  uint64  — universe size (original file size in bytes)
 *   [8-15]  k  uint64  — number of positions
 *   [16]    l  uint8   — number of lower bits per element (0-30)
 *   [17..]  lower_bytes — k*l bits, LSB-first sequential packing
 *   [..]    upper_bytes — unary buckets, LSB-first packing
 *
 * Returns 0 on success, -1 on OOM.
 */
int ef_encode(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t N, Buf *out);

/*
 * Decode an Elias-Fano block into a pre-allocated bitset.
 * flag_bs must be calloc'd to (N+7)/8 bytes by caller.
 * Returns 0 on success, -1 on error.
 */
int ef_decode(const uint8_t *data, size_t data_len,
              uint8_t *flag_bs, size_t flag_bs_len);

#endif /* ELIAS_FANO_H */

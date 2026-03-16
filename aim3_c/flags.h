#ifndef FLAGS_H
#define FLAGS_H

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "aim3.h"

/*
 * Encode flag bitset using gzip (gap or bitset mode).
 * flag_bs is (n+7)/8 bytes; bit i set iff position i had the target bit.
 * Sets *mode_out to FLAG_MODE_GAP_GZ or FLAG_MODE_BITSET.
 * Returns 0 on success, -1 on error.
 */
int encode_flags_gz(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                    int gz_level, Buf *out, int *mode_out);

/*
 * Encode flag bitset using Elias-Fano.
 * mode_out is always FLAG_MODE_EF.
 */
int encode_flags_ef(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                    Buf *out, int *mode_out);

/*
 * Encode flag bitset using Elias Gamma RLE.
 * mode_out is always FLAG_MODE_RLE.
 */
int encode_flags_rle(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                     Buf *out, int *mode_out);

/*
 * Try all three encoders, keep the smallest result.
 * Returns 0 on success, -1 on error.
 */
int encode_flags_best(const uint8_t *flag_bs, size_t flag_bs_len, uint64_t n,
                      int gz_level, Buf *out, int *mode_out);

/*
 * Decode a flag block into a pre-allocated bitset.
 * flag_bs must be calloc'd to (n+7)/8 bytes by caller (zeroed).
 * flag_mode must be one of FLAG_MODE_*.
 * Returns 0 on success, -1 on error.
 */
int decode_flags(const uint8_t *payload, size_t payload_len,
                 uint64_t n, int flag_mode,
                 uint8_t *flag_bs, size_t flag_bs_len);

#endif /* FLAGS_H */

/*
 * SHA-256 — Brad Conte's public-domain implementation (modified for aim3).
 * https://github.com/B-Con/crypto-algorithms
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE 32

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

/* Convenience: hash a buffer in one call. */
void sha256_buf(const uint8_t *data, size_t len, uint8_t hash[SHA256_BLOCK_SIZE]);

/* Convenience: hash a file in one call (streaming, no N-byte buffer).
 * Returns 0 on success, -1 if the file cannot be opened. */
int sha256_file(const char *path, uint8_t hash[SHA256_BLOCK_SIZE]);

#endif /* SHA256_H */

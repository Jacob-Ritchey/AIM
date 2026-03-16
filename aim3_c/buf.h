#ifndef BUF_H
#define BUF_H

#include <stddef.h>
#include <stdint.h>

/* Growable byte buffer. All encode functions return one of these. */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

/* Initialise to empty (stack-safe: just zero the struct). */
void buf_init(Buf *b);

/* Append bytes. Reallocs (doubles) on overflow. Returns 0 on success, -1 on OOM. */
int buf_append(Buf *b, const void *src, size_t n);

/* Append a single byte. */
int buf_push(Buf *b, uint8_t byte);

/* Reserve at least `n` total bytes of capacity without changing len. */
int buf_reserve(Buf *b, size_t n);

/* Reserve exactly `n` bytes of capacity — no doubling. Use when final size is known. */
int buf_reserve_exact(Buf *b, size_t n);

/* Free heap memory and reset to empty. Safe to call on a zero-initialised Buf. */
void buf_free(Buf *b);

/* Reverse the contents in-place (used for ANS output stream reversal). */
void buf_reverse(Buf *b);

/* Write a big-endian uint16 / uint32 / uint64 into a Buf. */
int buf_put_u16be(Buf *b, uint16_t v);
int buf_put_u32be(Buf *b, uint32_t v);
int buf_put_u64be(Buf *b, uint64_t v);

#endif /* BUF_H */

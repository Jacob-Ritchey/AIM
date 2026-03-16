#include "buf.h"
#include <stdlib.h>
#include <string.h>

void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

int buf_reserve(Buf *b, size_t n)
{
    if (n <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < n) nc *= 2;
    uint8_t *p = realloc(b->data, nc);
    if (!p) return -1;
    b->data = p; b->cap = nc;
    return 0;
}

int buf_reserve_exact(Buf *b, size_t n)
{
    if (n <= b->cap) return 0;
    uint8_t *p = realloc(b->data, n);
    if (!p) return -1;
    b->data = p; b->cap = n;
    return 0;
}

int buf_append(Buf *b, const void *src, size_t n)
{
    if (!n) return 0;
    if (buf_reserve(b, b->len + n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int buf_push(Buf *b, uint8_t byte)
{
    return buf_append(b, &byte, 1);
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}

void buf_reverse(Buf *b)
{
    if (b->len < 2) return;
    uint8_t *lo = b->data, *hi = b->data + b->len - 1;
    while (lo < hi) { uint8_t t = *lo; *lo++ = *hi; *hi-- = t; }
}

int buf_put_u16be(Buf *b, uint16_t v)
{
    uint8_t tmp[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return buf_append(b, tmp, 2);
}

int buf_put_u32be(Buf *b, uint32_t v)
{
    uint8_t tmp[4] = { (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v };
    return buf_append(b, tmp, 4);
}

int buf_put_u64be(Buf *b, uint64_t v)
{
    uint8_t tmp[8] = {
        (uint8_t)(v>>56),(uint8_t)(v>>48),(uint8_t)(v>>40),(uint8_t)(v>>32),
        (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v
    };
    return buf_append(b, tmp, 8);
}

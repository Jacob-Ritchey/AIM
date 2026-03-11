/* aim.c — AIM Compression Algorithm, C Reference Implementation
 * Version: 16  |  Reference: aim_v15.py  |  Spec: AIM_Specification_v16.docx
 *
 * v16 adds HALT_ANS_STRIDE (halt code 4): at each recursive depth the encoder
 * computes an rANS order-1 stride-k encoding of the aligned stream and stores
 * it as a candidate.  Bottom-up assembly picks the earliest depth where ANS-
 * stride is cheaper than the remaining subtree (optimal cutoff, no heuristic).
 * Stride k is selected by measuring H(X_i | X_{i-k}) over {1,2,3,4,6,8,12,16}.
 *
 * Build:  gcc -O3 -o aim aim.c -lz -lm
 * Usage:  ./aim encode <input> <o>
 *         ./aim decode <input> <o> [--no-verify]
 *         ./aim bench  <file>
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <zlib.h>

/* ── Types ────────────────────────────────────────────────────────────────── */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* Dynamic byte buffer */
typedef struct { u8 *d; size_t n, cap; } Buf;

static Buf buf_new(size_t cap) {
    Buf b; cap = cap ? cap : 64;
    b.d = (u8*)malloc(cap); b.n = 0; b.cap = cap; return b;
}
static void buf_grow(Buf *b, size_t need) {
    if (b->n + need > b->cap) {
        size_t nc = b->cap * 2; if (nc < b->n + need) nc = b->n + need;
        b->d = (u8*)realloc(b->d, nc); b->cap = nc;
    }
}
static void buf_push(Buf *b, u8 v)                       { buf_grow(b,1); b->d[b->n++]=v; }
static void buf_app (Buf *b, const u8 *p, size_t n)      { buf_grow(b,n); memcpy(b->d+b->n,p,n); b->n+=n; }
static void buf_free(Buf *b)                             { free(b->d); b->d=NULL; b->n=b->cap=0; }

/* BE read/write helpers */
static u32 rd32(const u8 *p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static u64 rd64(const u8 *p) { return ((u64)rd32(p)<<32)|rd32(p+4); }
static void wr32(u8 *p, u32 v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void wr64(u8 *p, u64 v) { wr32(p,(u32)(v>>32)); wr32(p+4,(u32)v); }


/* ── SHA-256 (public domain, Brad Conte) ─────────────────────────────────── */
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define RR(v,n) (((v)>>(n))|((v)<<(32-(n))))
#define S0(x)  (RR(x,2)^RR(x,13)^RR(x,22))
#define S1(x)  (RR(x,6)^RR(x,11)^RR(x,25))
#define s0(x)  (RR(x,7)^RR(x,18)^((x)>>3))
#define s1(x)  (RR(x,17)^RR(x,19)^((x)>>10))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))

static void sha256_transform(u32 st[8], const u8 *d) {
    u32 w[64]; int i;
    for (i=0;i<16;i++) w[i]=rd32(d+i*4);
    for (i=16;i<64;i++) w[i]=s1(w[i-2])+w[i-7]+s0(w[i-15])+w[i-16];
    u32 a=st[0],b=st[1],c=st[2],e=st[3],f=st[4],g=st[5],h=st[6],j=st[7];
    for (i=0;i<64;i++) {
        u32 t1=j+S1(f)+CH(f,g,h)+K256[i]+w[i];
        u32 t2=S0(a)+MJ(a,b,c);
        j=h; h=g; g=f; f=e+t1; e=c; c=b; b=a; a=t1+t2;
    }
    /* Note: variable shadowing deliberate — e/f/g/h/j map to d/e/f/g/h */
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=e;
    st[4]+=f; st[5]+=g; st[6]+=h; st[7]+=j;
}

static void sha256(const u8 *data, size_t len, u8 out[32]) {
    u32 st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8  buf[64]; size_t i, bl = len;
    for (i=0; i+64<=len; i+=64) sha256_transform(st, data+i);
    size_t rem = len - i;
    memcpy(buf, data+i, rem);
    buf[rem++] = 0x80;
    if (rem > 56) { memset(buf+rem,0,64-rem); sha256_transform(st,buf); rem=0; }
    memset(buf+rem, 0, 56-rem);
    u64 bits = (u64)bl << 3;
    wr64(buf+56, bits);
    sha256_transform(st, buf);
    for (i=0;i<8;i++) wr32(out+i*4, st[i]);
}


/* ── gzip via zlib ───────────────────────────────────────────────────────── */
/* Matches Python gzip.compress(data, 9) — RFC 1952 gzip container with
   DEFLATE payload at level 9. windowBits=15+16 activates gzip wrapping. */

static Buf gz_compress(const u8 *src, size_t sn) {
    z_stream z = {0};
    deflateInit2(&z, 9, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY);
    uLong bound = deflateBound(&z, (uLong)sn) + 64;
    Buf out = buf_new(bound);
    z.next_in  = (Bytef*)src;  z.avail_in  = (uInt)sn;
    z.next_out = (Bytef*)out.d; z.avail_out = (uInt)out.cap;
    deflate(&z, Z_FINISH);
    out.n = z.total_out;
    deflateEnd(&z);
    return out;
}

static Buf gz_decompress(const u8 *src, size_t sn) {
    z_stream z = {0};
    inflateInit2(&z, 15+32);  /* 15+32 = auto-detect gzip/zlib */
    Buf out = buf_new(sn * 4 + 256);
    z.next_in  = (Bytef*)src; z.avail_in = (uInt)sn;
    int r;
    do {
        if (out.n >= out.cap) { buf_grow(&out, out.cap); }
        z.next_out  = (Bytef*)(out.d + out.n);
        z.avail_out = (uInt)(out.cap - out.n);
        r = inflate(&z, Z_NO_FLUSH);
        out.n = z.total_out;
    } while (r == Z_OK || (r == Z_BUF_ERROR && z.avail_in > 0));
    inflateEnd(&z);
    return out;
}

/* Terminal codec: gzip (= LZ77 + structured Huffman / DEFLATE) */
static Buf term_encode(const u8 *d, size_t n) { return gz_compress(d, n); }
static Buf term_decode(const u8 *d, size_t n) { return gz_decompress(d, n); }


/* ── Canonical Huffman ───────────────────────────────────────────────────── */
/* Exact translation of Python _huff_build_lengths / _huff_codes /
   _huffman_encode / _huffman_decode.
   Key algorithm: heap of (freq, sym_list); merge by list concatenation;
   assign depths by recursive halving (syms[:n//2] / syms[n//2:]).
   Comparison order: freq first, then lex on sym list (matches Python heapq). */

/* ── Heap node: freq + symbol list ─────────────────────────────────────── */
typedef struct { u32 freq; u8 syms[256]; int nsyms; } HN;

static int hn_lt(const HN *a, const HN *b) {
    if (a->freq != b->freq) return a->freq < b->freq;
    int lim = a->nsyms < b->nsyms ? a->nsyms : b->nsyms;
    for (int i = 0; i < lim; i++)
        if (a->syms[i] != b->syms[i]) return a->syms[i] < b->syms[i];
    return a->nsyms < b->nsyms;
}

static void hn_push(HN *heap, int *hn, const HN *x) {
    heap[*hn] = *x;
    int i = (*hn)++;
    while (i > 0) {
        int p = (i-1)/2;
        if (hn_lt(&heap[i], &heap[p])) { HN t=heap[i]; heap[i]=heap[p]; heap[p]=t; i=p; }
        else break;
    }
}

static HN hn_pop(HN *heap, int *hn) {
    HN top = heap[0]; heap[0] = heap[--(*hn)];
    int i = 0;
    for (;;) {
        int l=2*i+1, r=2*i+2, s=i;
        if (l < *hn && hn_lt(&heap[l], &heap[s])) s=l;
        if (r < *hn && hn_lt(&heap[r], &heap[s])) s=r;
        if (s == i) break;
        HN t=heap[i]; heap[i]=heap[s]; heap[s]=t; i=s;
    }
    return top;
}

/* Recursive depth assignment: split at n//2, recurse each half +1 depth.
   Direct translation of Python's _assign(syms, depth). */
static void huff_assign(const u8 *syms, int n, int depth, u8 *lengths) {
    if (n == 1) {
        lengths[syms[0]] = (u8)(depth < 1 ? 1 : (depth > 15 ? 15 : depth));
        return;
    }
    int mid = n / 2;
    huff_assign(syms,       mid,   depth+1, lengths);
    huff_assign(syms+mid, n-mid, depth+1, lengths);
}

/* Build 256-byte length table. Exact translation of _huff_build_lengths. */
static void huff_build_lengths(const u8 *data, size_t n, u8 lengths[256]) {
    memset(lengths, 0, 256);
    if (n == 0) return;

    u32 freq[256] = {0};
    for (size_t i = 0; i < n; i++) freq[data[i]]++;

    /* Allocate heap on heap (256 nodes * sizeof(HN) ~= 66KB) */
    HN *heap = (HN*)malloc(257 * sizeof(HN));
    int hn = 0;

    /* Initial nodes: one per symbol with nonzero frequency.
       Python: [[f, [i]] for i, f in enumerate(freq) if f > 0]
       Iterating i=0..255 in order means sym 0 is pushed first, etc. */
    for (int i = 0; i < 256; i++) {
        if (freq[i]) {
            HN node; node.freq = freq[i]; node.syms[0] = (u8)i; node.nsyms = 1;
            hn_push(heap, &hn, &node);
        }
    }

    if (hn == 0) { free(heap); return; }
    if (hn == 1) { lengths[heap[0].syms[0]] = 1; free(heap); return; }

    /* Merge: pop two smallest, concatenate sym lists, push combined.
       Python: while len(heap) > 1: lo=pop(); hi=pop(); push([lo[0]+hi[0], lo[1]+hi[1]]) */
    while (hn > 1) {
        HN lo = hn_pop(heap, &hn);
        HN hi = hn_pop(heap, &hn);
        HN merged;
        merged.freq  = lo.freq + hi.freq;
        memcpy(merged.syms,             lo.syms, lo.nsyms);
        memcpy(merged.syms + lo.nsyms,  hi.syms, hi.nsyms);
        merged.nsyms = lo.nsyms + hi.nsyms;
        hn_push(heap, &hn, &merged);
    }

    /* Assign depths by recursive halving (Python: _assign(heap[0][1], 0)) */
    huff_assign(heap[0].syms, heap[0].nsyms, 0, lengths);
    free(heap);
}

/* Canonical code assignment: sort by (len, sym), assign codes in order.
   Direct translation of _huff_codes. */
static void huff_canonical_codes(const u8 *lengths, u32 codes[256], u8 clens[256]) {
    int order[256]; int cnt = 0;
    for (int i = 0; i < 256; i++) if (lengths[i]) order[cnt++] = i;
    /* Sort by (lengths[sym], sym) — matches Python sorted((l,i)...) */
    for (int i = 1; i < cnt; i++) {
        int key = order[i], j = i-1;
        while (j >= 0 && (lengths[order[j]] > lengths[key] ||
               (lengths[order[j]] == lengths[key] && order[j] > key))) {
            order[j+1] = order[j]; j--;
        }
        order[j+1] = key;
    }
    u32 code = 0; int prev_len = 0;
    for (int i = 0; i < cnt; i++) {
        int sym = order[i]; int l = lengths[sym];
        code <<= (l - prev_len);
        codes[sym] = code; clens[sym] = (u8)l;
        code++; prev_len = l;
    }
}

static Buf huffman_encode(const u8 *data, size_t n) {
    if (n == 0) {
        Buf out = buf_new(260);
        memset(out.d, 0, 256); wr32(out.d+256, 0); out.n = 260;
        return out;
    }

    u8 lengths[256] = {0};
    huff_build_lengths(data, n, lengths);

    u32 codes[256] = {0}; u8 clens[256] = {0};
    huff_canonical_codes(lengths, codes, clens);

    /* Count total bits */
    u64 total_bits = 0;
    for (size_t i = 0; i < n; i++) total_bits += clens[data[i]];

    size_t packed_bytes = ((size_t)total_bits + 7) / 8;
    Buf out = buf_new(260 + packed_bytes + 1);
    memcpy(out.d, lengths, 256); out.n = 256;
    wr32(out.d+256, (u32)total_bits); out.n += 4;
    buf_grow(&out, packed_bytes + 1);
    memset(out.d + 260, 0, packed_bytes);

    /* Pack bits MSB-first (Python: out[i>>3] |= 0x80 >> (i&7)) */
    u64 bit_pos = 0;
    for (size_t i = 0; i < n; i++) {
        u32 c = codes[data[i]]; int cl = clens[data[i]];
        for (int s = cl-1; s >= 0; s--) {
            if ((c >> s) & 1) out.d[260 + (bit_pos >> 3)] |= (u8)(0x80 >> (bit_pos & 7));
            bit_pos++;
        }
    }
    out.n = 260 + packed_bytes;
    return out;
}

static Buf huffman_decode(const u8 *payload, size_t plen) {
    if (plen < 260) { Buf b = buf_new(1); b.n = 0; return b; }
    u8 lengths[256]; memcpy(lengths, payload, 256);
    u32 n_bits = rd32(payload + 256);
    const u8 *packed = payload + 260;

    if (n_bits == 0) { Buf b = buf_new(1); b.n = 0; return b; }

    u32 codes[256] = {0}; u8 clens[256] = {0};
    huff_canonical_codes(lengths, codes, clens);

    /* Build flat decode table sorted by (len, code) for linear scan */
    int    ds[256]; u32 dc[256]; u8 dl[256]; int dn = 0;
    for (int i = 0; i < 256; i++)
        if (lengths[i]) { ds[dn]=i; dc[dn]=codes[i]; dl[dn]=(u8)clens[i]; dn++; }
    for (int i = 1; i < dn; i++) {
        int ks=ds[i]; u32 kc=dc[i]; u8 kl=dl[i]; int j=i-1;
        while (j >= 0 && (dl[j] > kl || (dl[j]==kl && dc[j]>kc)))
            { ds[j+1]=ds[j]; dc[j+1]=dc[j]; dl[j+1]=dl[j]; j--; }
        ds[j+1]=ks; dc[j+1]=kc; dl[j+1]=kl;
    }

    /* Estimate output: at minimum 1 bit per symbol */
    Buf out = buf_new((size_t)n_bits + 16);
    u32 cur_code = 0; int cur_len = 0;
    u64 bits_read = 0;
    size_t packed_bytes = ((size_t)n_bits + 7) / 8;

    for (size_t bi = 0; bi < packed_bytes && bits_read < n_bits; bi++) {
        u8 byte = packed[bi];
        for (int s = 7; s >= 0 && bits_read < n_bits; s--) {
            cur_code = (cur_code << 1) | ((byte >> s) & 1);
            cur_len++; bits_read++;
            for (int j = 0; j < dn; j++) {
                if (dl[j] == (u8)cur_len && dc[j] == cur_code) {
                    buf_push(&out, (u8)ds[j]);
                    cur_code = 0; cur_len = 0;
                    break;
                }
            }
        }
    }
    return out;
}



/* ── LZ77 ───────────────────────────────────────────────────────────────── */
/* Wire: orig_n(4B BE) + n_tokens(4B BE) + flag-grouped token stream.
   Per 8 tokens: flag byte (MSB=first). Match: dist(2B BE)+(len-3)(1B).
   Literal: byte(1B). Min match 3, max 258, window 32768, chain depth 16. */

#define LZ77_WIN   32768u
#define LZ77_MASK  (LZ77_WIN-1u)
#define LZ77_HBITS 16u
#define LZ77_HSIZ  (1u<<LZ77_HBITS)
#define LZ77_HMASK (LZ77_HSIZ-1u)
#define LZ77_MIN   3u
#define LZ77_MAX   258u
#define LZ77_CHAIN 16u
#define LZ77_EMPTY 0xFFFFFFFFu

static u32 lz77_hash(const u8 *p) {
    /* 3-byte hash to 16 bits */
    return (((u32)p[0]<<8) ^ (u32)p[1] ^ ((u32)p[2]<<4)) & LZ77_HMASK;
}

static Buf lz77_compress(const u8 *src, size_t sn) {
    u32 *head = (u32*)malloc(LZ77_HSIZ * sizeof(u32));
    u32 *prev = (u32*)malloc(LZ77_WIN  * sizeof(u32));
    for (u32 i=0;i<LZ77_HSIZ;i++) head[i]=LZ77_EMPTY;
    for (u32 i=0;i<LZ77_WIN; i++) prev[i]=LZ77_EMPTY;

    /* First pass: collect tokens */
    typedef struct { u8 is_match; u16 dist; u16 length; u8 lit; } Tok;
    size_t max_toks = sn + 8;
    Tok *toks = (Tok*)malloc(max_toks * sizeof(Tok));
    size_t ntoks = 0;

    size_t i = 0;
    while (i < sn) {
        if (i + LZ77_MIN > sn) {
            toks[ntoks++] = (Tok){0,0,0,src[i]}; i++; continue;
        }
        u32 h = lz77_hash(src+i);
        u32 j = head[h];
        u32 best_len=0, best_dist=0;
        u32 steps=0;
        while (j != LZ77_EMPTY && i > j && (i-j) <= LZ77_WIN && steps < LZ77_CHAIN) {
            u32 ml=0;
            while (i+ml<sn && src[j+ml]==src[i+ml] && ml<LZ77_MAX) ml++;
            if (ml > best_len) { best_len=ml; best_dist=(u32)(i-j); }
            j = prev[j & LZ77_MASK]; steps++;
        }
        prev[i & LZ77_MASK] = head[h];
        head[h] = (u32)i;
        if (best_len >= LZ77_MIN) {
            toks[ntoks++] = (Tok){1,(u16)best_dist,(u16)best_len,0};
            i += best_len;
        } else {
            toks[ntoks++] = (Tok){0,0,0,src[i]}; i++;
        }
    }

    /* Second pass: serialise */
    Buf out = buf_new(8 + ntoks*4 + 16);
    u8 hdr[8]; wr32(hdr,(u32)sn); wr32(hdr+4,(u32)ntoks); buf_app(&out,hdr,8);

    size_t ti=0;
    while (ti < ntoks) {
        size_t chunk = ntoks-ti < 8 ? ntoks-ti : 8;
        u8 flag=0;
        for (size_t j=0;j<chunk;j++) if (toks[ti+j].is_match) flag |= (1<<(7-j));
        buf_push(&out, flag);
        for (size_t j=0;j<chunk;j++) {
            Tok *t = &toks[ti+j];
            if (t->is_match) {
                u8 mb[3]; mb[0]=(u8)(t->dist>>8); mb[1]=(u8)t->dist; mb[2]=(u8)(t->length-LZ77_MIN);
                buf_app(&out,mb,3);
            } else {
                buf_push(&out, t->lit);
            }
        }
        ti += chunk;
    }
    free(head); free(prev); free(toks);
    return out;
}

static Buf lz77_decompress(const u8 *src, size_t sn) {
    if (sn < 8) { Buf b=buf_new(1); return b; }
    u32 orig_n  = rd32(src);
    u32 n_tokens= rd32(src+4);
    size_t pos=8;
    Buf out = buf_new(orig_n + 64);
    u32 tok=0;
    while (tok < n_tokens) {
        if (pos >= sn) break;
        u8 flag = src[pos++];
        for (int b=7; b>=0 && tok<n_tokens; b--) {
            if ((flag>>b)&1) {
                if (pos+3>sn) break;
                u32 dist = ((u32)src[pos]<<8)|src[pos+1]; pos+=2;
                u32 len  = src[pos++] + LZ77_MIN;
                size_t start = out.n - dist;
                for (u32 k=0;k<len;k++) buf_push(&out, out.d[start+k]);
            } else {
                if (pos>=sn) break;
                buf_push(&out, src[pos++]);
            }
            tok++;
        }
    }
    return out;
}


/* ── Elias-Fano ─────────────────────────────────────────────────────────── */
/* Wire: N(8B BE) + k(8B BE) + l(1B) + lower_bits + upper_bitvector.
   Bit packing is LSB-first throughout. */

static Buf ef_encode(const u32 *pos, size_t k, u64 N) {
    Buf out = buf_new(17 + (k*8)/8 + k/4 + 64);
    u8 hdr[17]; wr64(hdr,N); wr64(hdr+8,(u64)k);
    int l = (k==0||N==0) ? 0 : (int)floor(log2((double)N/k));
    if (l<0) l=0;
    if (l>30) l=30;
    hdr[16] = (u8)l;
    buf_app(&out, hdr, 17);

    if (k==0) return out;

    /* Lower bits (l bits per element, LSB-first packed) */
    size_t lower_bytes = l > 0 ? ((size_t)k * l + 7)/8 : 0;
    size_t lo_start = out.n;
    buf_grow(&out, lower_bytes);
    memset(out.d + lo_start, 0, lower_bytes);
    out.n += lower_bytes;

    if (l > 0) {
        for (size_t i=0;i<k;i++) {
            u32 low_val = pos[i] & ((1u<<l)-1u);
            size_t bo = (size_t)i * l;
            int bits_left=l; u32 val=low_val;
            while (bits_left>0) {
                size_t bi=bo>>3; int bb=bo&7; int chunk=8-bb; if(chunk>bits_left)chunk=bits_left;
                out.d[lo_start+bi] |= (u8)((val & ((1u<<chunk)-1u)) << bb);
                val>>=chunk; bo+=chunk; bits_left-=chunk;
            }
        }
    }

    /* Upper bitvector (unary, LSB-first) */
    size_t upper_size = k + (size_t)(N>>l) + 2;
    size_t upper_bytes = (upper_size+7)/8;
    size_t up_start = out.n;
    buf_grow(&out, upper_bytes);
    memset(out.d + up_start, 0, upper_bytes);
    out.n += upper_bytes;

    size_t bit_pos=0; size_t hi_idx=0; u64 bucket=0;
    while (hi_idx < k) {
        u32 hp = pos[hi_idx] >> l;
        while ((u64)hp == bucket) {
            size_t bi=bit_pos>>3; int bb=bit_pos&7;
            out.d[up_start+bi] |= (u8)(1<<bb);
            bit_pos++; hi_idx++;
            if (hi_idx>=k) goto ef_done;
            hp = pos[hi_idx] >> l;
        }
        bit_pos++; bucket++;
    }
ef_done:
    /* Trim upper bytes */
    out.n = up_start + (bit_pos+7)/8;
    return out;
}

static Buf ef_decode(const u8 *data, size_t dn) {
    if (dn < 17) { Buf b=buf_new(4); return b; }
    u64 k = rd64(data+8); u8 l = data[16]; (void)rd64(data);
    size_t pos = 17;

    Buf out = buf_new((size_t)k * sizeof(u32) + 4);

    if (k==0) return out;

    size_t lbc = l>0 ? ((size_t)k*l+7)/8 : 0;
    const u8 *lower_bytes = data+pos; pos += lbc;
    const u8 *upper_bytes = data+pos;
    size_t upper_len = dn - pos;

    /* Decode lower bits */
    u32 *lower = (u32*)calloc(k, sizeof(u32));
    if (l>0) {
        for (u64 i=0;i<k;i++) {
            u32 val=0; size_t bo=(size_t)i*l; int bits_left=l; int shift=0;
            while (bits_left>0) {
                size_t bi=bo>>3; int bb=bo&7; int chunk=8-bb; if(chunk>bits_left)chunk=bits_left;
                u8 bv = (bi<lbc) ? lower_bytes[bi] : 0;
                val |= (u32)((bv>>bb)&((1<<chunk)-1))<<shift;
                shift+=chunk; bo+=chunk; bits_left-=chunk;
            }
            lower[i] = val;
        }
    }

    /* Decode upper bits */
    u64 found=0, bucket=0; size_t bit_pos2=0;
    size_t total_bits = upper_len*8;
    u32 *res = (u32*)malloc((size_t)k*sizeof(u32));
    while (found<k && bit_pos2<total_bits) {
        size_t bi=bit_pos2>>3; int bb=bit_pos2&7;
        if (bi>=upper_len) break;
        if ((upper_bytes[bi]>>bb)&1) {
            res[found] = (u32)((bucket<<l)|lower[found]); found++;
        } else { bucket++; }
        bit_pos2++;
    }
    buf_app(&out, (u8*)res, (size_t)found*sizeof(u32));
    /* out.n = found*sizeof(u32): byte count, consistent with all other position bufs */
    free(lower); free(res);
    return out;
}


/* ── Gamma-coded RLE ─────────────────────────────────────────────────────── */
/* Wire: first_bit(1B) + n_runs(4B BE) + gamma-coded run lengths (MSB-first) */
/* Gamma: for x>=1: write floor(log2(x)) zero bits, 1 bit, then x - 2^floor(log2(x)) in binary */

static Buf gamma_encode(const u32 *runs, size_t nr) {
    Buf out = buf_new(nr*4+4);
    u32 bit_pos=0;
    for (size_t i=0;i<nr;i++) {
        u32 x = runs[i]; if(x<1)x=1;
        int k = 0; u32 tmp=x; while(tmp>1){k++;tmp>>=1;}
        /* write k zero bits */
        for (int j=0;j<k;j++) {
            size_t by=bit_pos>>3;
            if (by>=out.cap) buf_grow(&out,out.cap);
            if (by>=out.n) { while(out.n<=by) buf_push(&out,0); }
            /* 0 bit — already 0 */
            bit_pos++;
        }
        /* write k+1 data bits (x from MSB) */
        for (int j=k;j>=0;j--) {
            size_t by=bit_pos>>3; int bb=7-(bit_pos&7);
            if (by>=out.cap) buf_grow(&out,out.cap);
            if (by>=out.n) { while(out.n<=by) buf_push(&out,0); }
            if ((x>>j)&1) out.d[by] |= (u8)(1<<bb);
            bit_pos++;
        }
    }
    return out;
}

static size_t gamma_decode_one(const u8 *data, size_t dlen, size_t *bit_pos) {
    /* Count leading zeros */
    int k=0;
    while (*bit_pos < dlen*8) {
        size_t by=*bit_pos>>3; int bb=7-(*bit_pos&7);
        int bit = (data[by]>>bb)&1; (*bit_pos)++;
        if (!bit) k++;
        else break;
    }
    /* Read k more bits */
    u32 val=1;
    for (int j=0;j<k;j++) {
        if (*bit_pos >= dlen*8) break;
        size_t by=*bit_pos>>3; int bb=7-(*bit_pos&7);
        val = (val<<1)|((data[by]>>bb)&1); (*bit_pos)++;
    }
    return val;
}

static Buf rle_encode(const u32 *fp, size_t fk, size_t n) {
    if (fk==0) {
        u32 run_n = (u32)n;
        Buf gamma = gamma_encode(&run_n, 1);
        Buf out = buf_new(5 + gamma.n);
        buf_push(&out, 0); /* first_bit=0 */
        u8 tmp[4]; wr32(tmp,1); buf_app(&out,tmp,4);
        buf_app(&out, gamma.d, gamma.n);
        buf_free(&gamma);
        return out;
    }

    /* Build runs */
    u32 *runs = (u32*)malloc((fk*2+4)*sizeof(u32));
    size_t nr=0;
    int first_bit = (fp[0]>0) ? 0 : 1;

    if (first_bit==0) {
        runs[nr++] = (u32)fp[0]; /* initial zero-run */
        size_t i=0;
        while (i<fk) {
            /* one-run */
            size_t run_1=1;
            while (i+run_1<fk && fp[i+run_1]==fp[i]+run_1) run_1++;
            if (run_1>0) runs[nr++]=(u32)run_1;
            i+=run_1;
            if (i<fk) {
                u32 gap=(u32)(fp[i]-(fp[i-1]+1));
                if(gap>0) runs[nr++]=gap;
            }
        }
    } else {
        size_t i=0;
        while (i<fk) {
            size_t run_1=1;
            while (i+run_1<fk && fp[i+run_1]==fp[i]+run_1) run_1++;
            if (run_1>0) runs[nr++]=(u32)run_1;
            i+=run_1;
            if (i<fk) {
                u32 gap=(u32)(fp[i]-(fp[i-1]+1));
                if (gap>0) runs[nr++]=gap;
            }
        }
    }
    /* trailing zero-run */
    u32 trailing = (u32)(n - fp[fk-1] - 1);
    if (trailing>0) runs[nr++]=trailing;
    /* filter zero runs */
    size_t nr2=0;
    for (size_t i=0;i<nr;i++) if(runs[i]>0) runs[nr2++]=runs[i];
    nr=nr2;

    Buf gamma = gamma_encode(runs, nr);
    Buf out = buf_new(5 + gamma.n);
    buf_push(&out,(u8)first_bit);
    u8 tmp[4]; wr32(tmp,(u32)nr); buf_app(&out,tmp,4);
    buf_app(&out, gamma.d, gamma.n);
    buf_free(&gamma); free(runs);
    return out;
}

static Buf rle_decode(const u8 *data, size_t dn, size_t n) {
    if (dn<5) { Buf b=buf_new(4); return b; }
    int first_bit = data[0];
    u32 n_runs    = rd32(data+1);
    const u8 *gdata = data+5;
    size_t glen     = dn-5;

    u32 *runs = (u32*)malloc((n_runs+1)*sizeof(u32));
    size_t bit_pos=0;
    for (u32 i=0;i<n_runs;i++) runs[i]=(u32)gamma_decode_one(gdata,glen,&bit_pos);

    /* positions output */
    Buf out = buf_new(n/8+64);
    size_t pos=0; int cur=first_bit;
    for (u32 i=0;i<n_runs;i++) {
        u32 run=runs[i];
        if (cur==1) {
            /* ensure buffer capacity to store position as u32 */
            buf_grow(&out, run*sizeof(u32));
            for (u32 j=0;j<run;j++) {
                if (pos+j < n) {
                    u32 p2=(u32)(pos+j);
                    memcpy(out.d+out.n, &p2, sizeof(u32)); out.n+=sizeof(u32);
                }
            }
        }
        pos += run; cur ^= 1;
    }
    free(runs);
    return out; /* .n / sizeof(u32) = count of positions */
}


/* ── Bit operations ──────────────────────────────────────────────────────── */

static int sweep(const u8 *data, size_t n, int max_bit) {
    u64 counts[8]={0};
    for (size_t i=0;i<n;i++) {
        u8 v=data[i];
        for (int b=0;b<=max_bit;b++) if((v>>b)&1) counts[b]++;
    }
    int best=0;
    for (int b=1;b<=max_bit;b++) if(counts[b]<counts[best]) best=b;
    return best;
}

/* Returns flag position list (u32 array) and writes aligned bytes into *out_aligned.
   out_aligned must be pre-allocated to n bytes. */
static Buf bit_clear(const u8 *data, size_t n, int bit, u8 *out_aligned) {
    u8 mask=(u8)(1<<bit), inv=(u8)(~mask);
    Buf fp = buf_new(n/8+64);
    for (size_t i=0;i<n;i++) {
        if (data[i]&mask) {
            out_aligned[i]=data[i]&inv;
            u32 p=(u32)i; buf_grow(&fp,sizeof(u32)); memcpy(fp.d+fp.n,&p,sizeof(u32)); fp.n+=sizeof(u32);
        } else {
            out_aligned[i]=data[i];
        }
    }
    return fp; /* .n / sizeof(u32) = count */
}

static void remap(const u8 *in, u8 *out, size_t n, int bit) {
    u32 lo=(1u<<bit)-1u;
    for (size_t i=0;i<n;i++) {
        u8 v=in[i];
        out[i]=(u8)(((v>>(bit+1))<<bit)|(v&lo));
    }
}

static void unremap(const u8 *in, u8 *out, size_t n, int bit) {
    u32 lo=(1u<<bit)-1u;
    for (size_t i=0;i<n;i++) {
        u8 v=in[i];
        out[i]=(u8)(((v>>bit)<<(bit+1))|(v&lo));
    }
}

static void reconstruct(u8 *stream, const u32 *fp, size_t fk, int bit) {
    u8 mask=(u8)(1<<bit);
    for (size_t i=0;i<fk;i++) stream[fp[i]]|=mask;
}

/* Build packed bitset: bit (pos&7) of byte (pos>>3) = 1 for each pos in fp */
static Buf build_bitset(const u32 *fp, size_t fk, size_t n) {
    size_t bsz = (n+7)/8;
    Buf out = buf_new(bsz);
    memset(out.d, 0, bsz); out.n = bsz;
    for (size_t i=0;i<fk;i++) {
        u32 p=fp[i]; out.d[p>>3] |= (u8)(1<<(p&7));
    }
    return out;
}

/* Extract flag positions from a packed bitset */
static Buf bitset_to_positions(const u8 *bs, size_t n) {
    Buf out = buf_new((n/8+4)*sizeof(u32));
    for (size_t i=0;i<n;i++) {
        if ((bs[i>>3]>>(i&7))&1) {
            u32 p=(u32)i; buf_grow(&out,sizeof(u32));
            memcpy(out.d+out.n,&p,sizeof(u32)); out.n+=sizeof(u32);
        }
    }
    return out;
}

static int is_all_zero(const u8 *d, size_t n) {
    for (size_t i=0;i<n;i++) if(d[i]) return 0;
    return 1;
}
static int is_all_one(const u8 *d, size_t n) {
    for (size_t i=0;i<n;i++) if(d[i]!=1) return 0;
    return 1;
}


/* ── Constants ───────────────────────────────────────────────────────────── */
static const u8  AIM_MAGIC[4]   = {'A','I','M','4'};
#define HEADER_SIZE   45u   /* 4+1+8+32 */
#define MODE_RECURSIVE 0
#define MODE_CAIM      1
#define FMT_GAP      0
#define FMT_BITSET   1
#define FMT_EF       2
#define FMT_RLE      3
#define FMT_HUFFMAN  4
#define FMT_LZ77     5
#define FMT_LZ77HUFF 6
#define HALT_RECURSE  0
#define HALT_TERMINAL 1
#define HALT_ZERO     2
#define HALT_ONE        3
#define HALT_ANS_STRIDE 4
#define MAX_DEPTH      8
#define EF_MAX_DENSITY  0.15
#define GAP_MAX_DENSITY 0.30

/* ── rANS constants ───────────────────────────────────────────────────────────────────────────────── */
#define ANS_M_BITS   14
#define ANS_M        (1u << ANS_M_BITS)   /* 16384 */
#define ANS_L        ANS_M
#define ANS_B        256u
#define STRIDE_SAMPLE      1000000
#define STRIDE_GAIN_THRESH 0.05

/* Per-context (or global) frequency / cumulative / slot table.
   Heap-allocated; one per context byte that has >= 32 observations. */
typedef struct {
    u16 freq[256];
    u32 cum[256];
    u8  slots[ANS_M];
} AnsCtx;


/* ── Flag format encode/decode ───────────────────────────────────────────── */

static Buf encode_fmt(const u32 *fp, size_t fk, size_t n, int fmt) {
    if (fmt==FMT_BITSET || fmt==FMT_HUFFMAN || fmt==FMT_LZ77 || fmt==FMT_LZ77HUFF) {
        Buf bs = build_bitset(fp, fk, n);
        Buf out;
        if (fmt==FMT_BITSET)   { return bs; }
        if (fmt==FMT_HUFFMAN)  { out = huffman_encode(bs.d, bs.n); buf_free(&bs); return out; }
        if (fmt==FMT_LZ77)     { out = lz77_compress(bs.d, bs.n);  buf_free(&bs); return out; }
        /* FMT_LZ77HUFF */       out = gz_compress(bs.d, bs.n);    buf_free(&bs); return out;
    }
    if (fmt==FMT_GAP) {
        if (fk==0) { Buf out=buf_new(4); u8 z[4]={0,0,0,0}; buf_app(&out,z,4); return out; }
        /* Compute max gap */
        u32 max_gap=fp[0];
        for (size_t i=1;i<fk;i++) { u32 g=fp[i]-fp[i-1]; if(g>max_gap) max_gap=g; }
        u8 width; u32 k_field=(u32)fk;
        if (max_gap<=255)      { width=1; }
        else if (max_gap<=65534){ width=2; k_field|=0x80000000u; }
        else                   { width=4; k_field|=0x40000000u; }
        Buf out=buf_new(4+fk*width+4);
        u8 tmp[4]; wr32(tmp,k_field); buf_app(&out,tmp,4);
        u32 prev=0;
        for (size_t i=0;i<fk;i++) {
            u32 g = (i==0) ? fp[0] : fp[i]-fp[i-1];
            if (width==1) buf_push(&out,(u8)g);
            else if (width==2) { u8 b[2]={(u8)(g>>8),(u8)g}; buf_app(&out,b,2); }
            else { u8 b[4]; wr32(b,g); buf_app(&out,b,4); }
            (void)prev;
        }
        return out;
    }
    if (fmt==FMT_EF) return ef_encode(fp, fk, (u64)n);
    if (fmt==FMT_RLE) return rle_encode(fp, fk, n);
    /* fallback */
    Buf b=buf_new(1); return b;
}

/* Decode flag format → position array. Returns Buf where .n/sizeof(u32) = count */
static Buf decode_fmt(const u8 *data, size_t dn, size_t n, int fmt) {
    if (fmt==FMT_BITSET || fmt==FMT_HUFFMAN || fmt==FMT_LZ77 || fmt==FMT_LZ77HUFF) {
        Buf bs;
        if      (fmt==FMT_BITSET)   { bs.d=(u8*)data; bs.n=dn; bs.cap=0; /* no free */ }
        else if (fmt==FMT_HUFFMAN)  { bs = huffman_decode(data, dn); }
        else if (fmt==FMT_LZ77)     { bs = lz77_decompress(data, dn); }
        else                        { bs = gz_decompress(data, dn); }
        Buf positions = bitset_to_positions(bs.d, n);
        if (fmt!=FMT_BITSET) buf_free(&bs);
        return positions;
    }
    if (fmt==FMT_GAP) {
        if (dn<4) { Buf b=buf_new(4); return b; }
        u32 k_field=rd32(data);
        int four_b=(k_field>>30)==3, two_b=(k_field>>31)&1&&!four_b;
        u32 k=k_field&0x3FFFFFFFu;
        Buf out=buf_new(k*sizeof(u32)+4);
        if(k==0) return out;
        size_t pos=4; u32 cur=0;
        for (u32 i=0;i<k;i++) {
            u32 g;
            if (four_b) { g=rd32(data+pos); pos+=4; }
            else if (two_b) { g=((u32)data[pos]<<8)|data[pos+1]; pos+=2; }
            else { g=data[pos]; pos++; }
            cur+=g;
            buf_grow(&out,sizeof(u32)); memcpy(out.d+out.n,&cur,sizeof(u32)); out.n+=sizeof(u32);
        }
        return out;
    }
    if (fmt==FMT_EF) {
        Buf raw = ef_decode(data, dn);
        /* raw.n = count of u32 elements */
        return raw;
    }
    if (fmt==FMT_RLE) {
        return rle_decode(data, dn, n);
    }
    Buf b=buf_new(4); return b;
}



/* ── rANS order-1 stride-k codec ────────────────────────────────────────────
 *
 * NORMALISE
 * ---------
 * Given raw_freq[256] (u32 counts), produce freq (u16, sums to ANS_M),
 * cum (u32 prefix sums), and slots[ANS_M] (decode lookup: slot -> symbol).
 * Zero-count symbols get freq=0 and do not occupy any slots.
 */
static void ans_normalise(const u32 *raw, AnsCtx *ctx) {
    u64 total = 0;
    for (int i = 0; i < 256; i++) total += raw[i];

    if (total == 0) {
        memset(ctx->freq,  0, sizeof(ctx->freq));
        memset(ctx->cum,   0, sizeof(ctx->cum));
        memset(ctx->slots, 0, sizeof(ctx->slots));
        return;
    }

    /* Initial rounded frequencies (at least 1 for any symbol present) */
    int64_t f64[256]; int64_t sum = 0;
    for (int i = 0; i < 256; i++) {
        if (raw[i] > 0) {
            int64_t v = (int64_t)round((double)raw[i] * ANS_M / (double)total);
            f64[i] = v < 1 ? 1 : v;
        } else {
            f64[i] = 0;
        }
        sum += f64[i];
    }

    /* Adjust so sum == ANS_M exactly.
       Sort active symbols by descending frequency (match Python behaviour). */
    int active[256]; int na = 0;
    for (int i = 0; i < 256; i++) if (f64[i] > 0) active[na++] = i;
    /* insertion sort descending by f64 */
    for (int i = 1; i < na; i++) {
        int key = active[i]; int j = i - 1;
        while (j >= 0 && f64[active[j]] < f64[key]) { active[j+1]=active[j]; j--; }
        active[j+1] = key;
    }
    int64_t delta = (int64_t)ANS_M - sum;
    int idx = 0;
    while (delta != 0) {
        int s = active[idx % na];
        if (delta > 0)              { f64[s]++; delta--; }
        else if (f64[s] > 1)        { f64[s]--; delta++; }
        idx++;
    }

    /* Build cum and slots */
    u32 acc = 0;
    for (int s = 0; s < 256; s++) {
        ctx->cum[s]  = acc;
        ctx->freq[s] = (u16)f64[s];
        for (u32 j = 0; j < (u32)f64[s]; j++) ctx->slots[acc + j] = (u8)s;
        acc += (u32)f64[s];
    }
}

/* CONDITIONAL ENTROPY  H(X_i | X_{i-k}) */
static double ans_cond_entropy(const u8 *data, size_t n, int k) {
    if ((size_t)k >= n) return 8.0;
    u32 *pair = (u32*)calloc(256*256, sizeof(u32));
    u32  ctx_c[256] = {0};
    size_t total = n - (size_t)k;
    for (size_t i = (size_t)k; i < n; i++) {
        u8 c = data[i-k], s = data[i];
        pair[(int)c*256 + s]++;
        ctx_c[(int)c]++;
    }
    double H = 0.0;
    for (int c = 0; c < 256; c++) {
        if (!ctx_c[c]) continue;
        for (int s = 0; s < 256; s++) {
            u32 cnt = pair[c*256+s];
            if (!cnt) continue;
            double p   = (double)cnt / (double)total;
            double pgc = (double)cnt / (double)ctx_c[c];
            H -= p * log2(pgc);
        }
    }
    free(pair);
    return H;
}

/* SELECT STRIDE  k_opt = argmin_k H(X_i | X_{i-k})
   Falls back to k=1 if gain is below STRIDE_GAIN_THRESH. */
static int ans_select_stride(const u8 *data, size_t n) {
    static const int cands[] = {1,2,3,4,6,8,12,16};
    size_t probe = n > STRIDE_SAMPLE ? (size_t)STRIDE_SAMPLE : n;
    double H1      = ans_cond_entropy(data, probe, 1);
    double best_H  = H1;
    int    k_opt   = 1;
    for (int ci = 0; ci < 8; ci++) {
        int k = cands[ci];
        double H = ans_cond_entropy(data, probe, k);
        if (H < best_H) { best_H = H; k_opt = k; }
    }
    if (H1 - best_H < STRIDE_GAIN_THRESH) k_opt = 1;
    return k_opt;
}

/*
 * ANS STRIDE ENCODE
 * -----------------
 * Wire format:
 *   k_opt       (1B)
 *   n           (4B BE)
 *   global_freq (512B = 256 × u16 LE)
 *   n_ctx       (2B BE)
 *   n_ctx × [ ctx_byte(1B) + freq(512B = 256 × u16 LE) ]
 *   payload_len (4B BE)
 *   payload     state(4B BE) + renorm_bytes(LSB-first emission order)
 *
 * The global table is stored as a fixed 512-byte block BEFORE per-context
 * tables to prevent collision with ctx_byte = 255.
 */
static Buf ans_stride_encode(const u8 *data, size_t n) {
    /* Handle empty */
    if (n == 0) {
        Buf out = buf_new(4 + 512 + 2 + 4);  /* hdr + global + n_ctx=0 + plen=0 */
        u8 tmp[4]; wr32(tmp, 0); buf_push(&out, 1); buf_app(&out, tmp, 4);
        u8 gzero[512] = {0}; buf_app(&out, gzero, 512);
        u8 nc[2]={0,0}; buf_app(&out,nc,2);
        u8 pl[4]={0,0,0,0}; buf_app(&out,pl,4);
        return out;
    }

    int k = ans_select_stride(data, n);

    /* Build global raw counts */
    u32 global_raw[256] = {0};
    for (size_t i = 0; i < n; i++) global_raw[(int)data[i]]++;

    /* Build per-context raw counts: pair[ctx][sym] */
    u32 *pair = (u32*)calloc(256*256, sizeof(u32));
    for (size_t i = (size_t)k; i < n; i++)
        pair[(int)data[i-k]*256 + (int)data[i]]++;

    /* Count per-context totals */
    u32 ctx_total[256] = {0};
    for (int c = 0; c < 256; c++)
        for (int s = 0; s < 256; s++)
            ctx_total[c] += pair[c*256+s];

    /* Normalise global */
    AnsCtx *gctx = (AnsCtx*)malloc(sizeof(AnsCtx));
    ans_normalise(global_raw, gctx);

    /* Normalise per-context tables (>= 32 observations) */
    AnsCtx *pctx[256]; memset(pctx, 0, sizeof(pctx));
    size_t n_ctx = 0;
    for (int c = 0; c < 256; c++) {
        if (ctx_total[c] >= 32) {
            pctx[c] = (AnsCtx*)malloc(sizeof(AnsCtx));
            ans_normalise(pair + c*256, pctx[c]);
            n_ctx++;
        }
    }
    free(pair);

    /* Encode: reverse symbol order, renorm into 'renorm' buffer */
    Buf renorm = buf_new(n + 16);
    u64 x = ANS_L;   /* use u64 to avoid overflow during limit computation */
    for (size_t ii = n; ii-- > 0; ) {
        u8 s   = data[ii];
        int c  = (ii >= (size_t)k) ? (int)data[ii-k] : 256; /* 256 = sentinel */
        AnsCtx *t = (c < 256) ? pctx[c] : NULL;
        u32 f_s = t ? t->freq[s] : gctx->freq[s];
        u32 c_s = t ? t->cum[s]  : gctx->cum[s];
        if (f_s == 0) { f_s = 1; c_s = 0; } /* safety: shouldn't happen */
        u64 limit = ((u64)ANS_L * ANS_B / ANS_M) * f_s;
        while (x >= limit) { buf_push(&renorm, (u8)(x & 0xFF)); x >>= 8; }
        x = (x / f_s) * ANS_M + c_s + (x % f_s);
    }

    /* Payload = state(4B BE) + renorm_bytes */
    u8 state_be[4];
    state_be[0]=(u8)(x>>24); state_be[1]=(u8)(x>>16);
    state_be[2]=(u8)(x>>8);  state_be[3]=(u8)(x);
    Buf payload = buf_new(4 + renorm.n);
    buf_app(&payload, state_be, 4);
    buf_app(&payload, renorm.d, renorm.n);
    buf_free(&renorm);

    /* Serialise header */
    Buf out = buf_new(1 + 4 + 512 + 2 + n_ctx*513 + 4 + payload.n);

    /* k_opt + n */
    buf_push(&out, (u8)k);
    { u8 tmp[4]; wr32(tmp, (u32)n); buf_app(&out, tmp, 4); }

    /* Global freq table: 256 × u16 LE */
    for (int s = 0; s < 256; s++) {
        buf_push(&out, (u8)(gctx->freq[s] & 0xFF));
        buf_push(&out, (u8)(gctx->freq[s] >> 8));
    }

    /* n_ctx (2B BE) */
    buf_push(&out, (u8)((n_ctx >> 8) & 0xFF));
    buf_push(&out, (u8)(n_ctx & 0xFF));

    /* Per-context tables */
    for (int c = 0; c < 256; c++) {
        if (!pctx[c]) continue;
        buf_push(&out, (u8)c);
        for (int s = 0; s < 256; s++) {
            buf_push(&out, (u8)(pctx[c]->freq[s] & 0xFF));
            buf_push(&out, (u8)(pctx[c]->freq[s] >> 8));
        }
        free(pctx[c]); pctx[c] = NULL;
    }

    /* payload_len + payload */
    { u8 tmp[4]; wr32(tmp, (u32)payload.n); buf_app(&out, tmp, 4); }
    buf_app(&out, payload.d, payload.n);
    buf_free(&payload);
    free(gctx);
    return out;
}

/*
 * ANS STRIDE DECODE
 * -----------------
 * Inverse of ans_stride_encode.  Returns the original aligned stream.
 */
static Buf ans_stride_decode(const u8 *data, size_t dn) {
    if (dn < 7) { Buf b = buf_new(1); b.n = 0; return b; }
    size_t pos = 0;
    int k  = (int)data[pos++];
    u32 n  = rd32(data + pos); pos += 4;

    if (n == 0) { Buf b = buf_new(1); b.n = 0; return b; }

    /* Read global table (512 bytes = 256 × u16 LE) */
    u32 graw[256] = {0};
    for (int s = 0; s < 256; s++) {
        graw[s] = (u32)data[pos] | ((u32)data[pos+1] << 8); pos += 2;
    }
    AnsCtx *gctx = (AnsCtx*)malloc(sizeof(AnsCtx));
    ans_normalise(graw, gctx);

    /* Read per-context tables */
    u32 nc_raw = ((u32)data[pos] << 8) | data[pos+1]; pos += 2;
    AnsCtx *pctx[256]; memset(pctx, 0, sizeof(pctx));
    for (u32 i = 0; i < nc_raw; i++) {
        int c = (int)data[pos++];
        u32 raw[256] = {0};
        for (int s = 0; s < 256; s++) {
            raw[s] = (u32)data[pos] | ((u32)data[pos+1] << 8); pos += 2;
        }
        pctx[c] = (AnsCtx*)malloc(sizeof(AnsCtx));
        ans_normalise(raw, pctx[c]);
    }

    /* Read payload: state(4B BE) + renorm_bytes */
    u32 plen = rd32(data + pos); pos += 4;
    if (pos + plen > dn || plen < 4) {
        /* malformed */
        free(gctx);
        for (int c = 0; c < 256; c++) if (pctx[c]) free(pctx[c]);
        Buf b = buf_new(1); b.n = 0; return b;
    }
    const u8 *payload = data + pos;

    /* Initial ANS state (big-endian from first 4 bytes) */
    u64 x = rd32(payload);

    /* Reversed renorm bytes */
    size_t rlen = plen - 4;
    u8 *buf = (u8*)malloc(rlen + 1);
    for (size_t i = 0; i < rlen; i++) buf[i] = payload[4 + rlen - 1 - i];
    size_t rpos = 0;

    u8 *out_d = (u8*)malloc(n);
    for (u32 i = 0; i < n; i++) {
        int c = (i >= (u32)k) ? (int)out_d[i-k] : 256; /* sentinel */
        AnsCtx *t = (c < 256) ? pctx[c] : NULL;
        const u8  *slots = t ? t->slots : gctx->slots;
        const u16 *freq  = t ? t->freq  : gctx->freq;
        const u32 *cum   = t ? t->cum   : gctx->cum;

        u32 slot = (u32)(x % ANS_M);
        u8  s    = slots[slot];
        out_d[i] = s;
        x = (u64)freq[s] * (x >> ANS_M_BITS) + slot - cum[s];
        while (x < ANS_L && rpos < rlen) x = (x << 8) | buf[rpos++];
    }
    free(buf);
    free(gctx);
    for (int c = 0; c < 256; c++) if (pctx[c]) { free(pctx[c]); pctx[c] = NULL; }

    Buf out; out.d = out_d; out.n = n; out.cap = n;
    return out;
}


/* ── Flag race ───────────────────────────────────────────────────────────── */

static Buf flag_race(const u32 *fp, size_t fk, size_t n, int *out_fmt) {
    double density = n ? (double)fk/n : 0.0;

    /* Formats to try */
    int fmts[7]; int nf=0;
    fmts[nf++]=FMT_HUFFMAN;
    fmts[nf++]=FMT_LZ77HUFF;
    fmts[nf++]=FMT_RLE;
    fmts[nf++]=FMT_BITSET;
    if (density <= GAP_MAX_DENSITY) { fmts[nf++]=FMT_GAP; fmts[nf++]=FMT_EF; }
    if (density >= 0.15 && density <= 0.85) fmts[nf++]=FMT_LZ77;

    Buf best; best.d=NULL; best.n=SIZE_MAX; best.cap=0;
    int best_fmt=FMT_BITSET;

    for (int i=0;i<nf;i++) {
        Buf candidate = encode_fmt(fp, fk, n, fmts[i]);
        if (candidate.n < best.n) {
            if (best.d) buf_free(&best);
            best = candidate; best_fmt = fmts[i];
        } else {
            buf_free(&candidate);
        }
    }
    *out_fmt = best_fmt;
    return best;
}


/* ── Recursive encode ────────────────────────────────────────────────────── */
/* Level header wire: bit(1) fmt(1) flag_len(4BE) halt(1) child_len(4BE) flag_data */

typedef struct { int bit, fmt; Buf flag_block; Buf ans_candidate; } Level;

static Buf recursive_encode(const u8 *data, size_t n) {
    if (n==0) {
        Buf term = gz_compress(NULL, 0);
        Buf out = buf_new(11 + term.n);
        u8 hdr[11]={0,FMT_GAP,0,0,0,0,HALT_TERMINAL,0,0,0,0};
        wr32(hdr+7,(u32)term.n);
        buf_app(&out,hdr,11); buf_app(&out,term.d,term.n);
        buf_free(&term); return out;
    }

    Level levels[MAX_DEPTH];
    int   ndepth=0;

    u8 *current  = (u8*)malloc(n);
    u8 *next_buf = (u8*)malloc(n);
    memcpy(current, data, n);

    for (int depth=0; depth<MAX_DEPTH; depth++) {
        int max_bit = 7 - depth;
        if (max_bit < 0) break;
        if (is_all_zero(current,n) || is_all_one(current,n)) break;

        int bit = sweep(current, n, max_bit);

        u8 *aligned = (u8*)malloc(n);
        Buf fp = bit_clear(current, n, bit, aligned);
        size_t fk = fp.n / sizeof(u32);

        /* v16: compute ANS-stride candidate on aligned stream (pre-remap).
           This is where inter-symbol value correlations are still intact. */
        Buf ans_cand = ans_stride_encode(aligned, n);

        int fmt;
        Buf flag_block = flag_race((u32*)fp.d, fk, n, &fmt);
        buf_free(&fp);

        levels[ndepth].bit           = bit;
        levels[ndepth].fmt           = fmt;
        levels[ndepth].flag_block    = flag_block;
        levels[ndepth].ans_candidate = ans_cand;
        ndepth++;

        remap(aligned, next_buf, n, bit);
        free(aligned);
        memcpy(current, next_buf, n);
    }
    free(next_buf);

    /* Build terminal */
    Buf inner;
    if (is_all_zero(current,n)) {
        inner = buf_new(11);
        u8 hdr[11]={0,FMT_GAP,0,0,0,0,HALT_ZERO,0,0,0,0};
        buf_app(&inner,hdr,11);
    } else if (is_all_one(current,n)) {
        inner = buf_new(11);
        u8 hdr[11]={0,FMT_GAP,0,0,0,0,HALT_ONE,0,0,0,0};
        buf_app(&inner,hdr,11);
    } else {
        Buf term = term_encode(current, n);
        inner = buf_new(11 + term.n);
        u8 hdr[11]={0,FMT_GAP,0,0,0,0,HALT_TERMINAL,0,0,0,0};
        wr32(hdr+7,(u32)term.n);
        buf_app(&inner,hdr,11); buf_app(&inner,term.d,term.n);
        buf_free(&term);
    }
    free(current);

    /* Bottom-up assembly with optimal ANS cutoff.
     *
     * At each depth (deepest first) compare two costs:
     *   option_recurse    = len(current payload)     — everything below
     *   option_ans_stride = 11 + len(ans_candidate)  — halt here with ANS
     *
     * Emit HALT_ANS_STRIDE when it is strictly smaller; otherwise HALT_RECURSE.
     * The first depth where ANS wins becomes the cutoff; all deeper levels were
     * already folded into 'payload' which gets replaced. */
    Buf payload = inner;
    for (int i = ndepth-1; i >= 0; i--) {
        size_t ans_cost     = 11 + levels[i].ans_candidate.n;
        size_t recurse_cost = payload.n;

        u8 hdr[11];
        hdr[0] = (u8)levels[i].bit;
        hdr[1] = (u8)levels[i].fmt;
        wr32(hdr+2, (u32)levels[i].flag_block.n);

        if (ans_cost < recurse_cost) {
            /* ANS wins at this depth — discard the subtree below */
            hdr[6] = HALT_ANS_STRIDE;
            wr32(hdr+7, (u32)levels[i].ans_candidate.n);
            buf_free(&payload);
            Buf np = buf_new(11 + levels[i].flag_block.n + levels[i].ans_candidate.n);
            buf_app(&np, hdr, 11);
            buf_app(&np, levels[i].flag_block.d,    levels[i].flag_block.n);
            buf_app(&np, levels[i].ans_candidate.d, levels[i].ans_candidate.n);
            payload = np;
        } else {
            hdr[6] = HALT_RECURSE;
            wr32(hdr+7, (u32)payload.n);
            Buf np = buf_new(11 + levels[i].flag_block.n + payload.n);
            buf_app(&np, hdr, 11);
            buf_app(&np, levels[i].flag_block.d, levels[i].flag_block.n);
            buf_app(&np, payload.d, payload.n);
            buf_free(&payload);
            payload = np;
        }
        buf_free(&levels[i].flag_block);
        buf_free(&levels[i].ans_candidate);
    }
    return payload;
}


/* ── Recursive decode ────────────────────────────────────────────────────── */

static Buf recursive_decode(const u8 *payload, size_t plen, size_t n) {
    if (n==0) { Buf b=buf_new(1); return b; }

    typedef struct { int bit, fmt; const u8 *fd; size_t fdlen; } DLevel;
    DLevel dlevels[MAX_DEPTH]; int ndepth=0;
    size_t pos=0;
    u8 *current=NULL;
    int ans_terminal=0;   /* 1 if deepest halt was HALT_ANS_STRIDE */

    while (pos+11<=plen) {
        int bit   = payload[pos];
        int fmt   = payload[pos+1];
        u32 flen  = rd32(payload+pos+2);
        int halt  = payload[pos+6];
        u32 clen  = rd32(payload+pos+7);
        const u8 *fd = payload+pos+11;
        pos += 11 + flen;

        if (halt==HALT_RECURSE) {
            dlevels[ndepth].bit=bit; dlevels[ndepth].fmt=fmt;
            dlevels[ndepth].fd=fd;  dlevels[ndepth].fdlen=flen;
            ndepth++;
        } else {
            const u8 *child = payload+pos;
            if (halt==HALT_ZERO) {
                current=(u8*)calloc(n,1);
            } else if (halt==HALT_ONE) {
                current=(u8*)malloc(n); memset(current,1,n);
            } else if (halt==HALT_ANS_STRIDE) {
                /* child is the self-describing ANS-stride payload.
                   Result is the aligned stream (post-bit-clear, pre-remap).
                   Do NOT unremap this level during reconstruction. */
                Buf t = ans_stride_decode(child, (size_t)clen);
                current=(u8*)malloc(t.n); memcpy(current,t.d,t.n); buf_free(&t);
                ans_terminal=1;
            } else { /* HALT_TERMINAL */
                Buf t = term_decode(child, (size_t)clen);
                current=(u8*)malloc(t.n); memcpy(current,t.d,t.n); buf_free(&t);
            }
            if (flen>0) {
                dlevels[ndepth].bit=bit; dlevels[ndepth].fmt=fmt;
                dlevels[ndepth].fd=fd;  dlevels[ndepth].fdlen=flen;
                ndepth++;
            }
            break;
        }
    }

    if (!current) { current=(u8*)calloc(n,1); }

    /* Rebuild bottom-up.
     * For HALT_ANS_STRIDE: the deepest level's `current` is already the aligned
     * stream (bit-cleared, not remapped), so skip unremap for that level only.
     * All shallower levels proceed normally: unremap then reconstruct. */
    u8 *tmp = (u8*)malloc(n);
    for (int i=ndepth-1; i>=0; i--) {
        int is_ans_level = ans_terminal && (i == ndepth-1);
        if (is_ans_level) {
            /* current IS aligned — skip unremap, reconstruct directly */
            memcpy(tmp, current, n);
        } else {
            unremap(current, tmp, n, dlevels[i].bit);
        }
        Buf fp = decode_fmt(dlevels[i].fd, dlevels[i].fdlen, n, dlevels[i].fmt);
        size_t fk = fp.n/sizeof(u32);
        reconstruct(tmp, (u32*)fp.d, fk, dlevels[i].bit);
        buf_free(&fp);
        u8 *swap=current; current=tmp; tmp=swap;
    }
    free(tmp);

    Buf out; out.d=current; out.n=n; out.cap=n;
    return out;
}


/* ── CAIM encode ─────────────────────────────────────────────────────────── */

static Buf caim_encode(const u8 *data, size_t n) {
    u8  which_bits[MAX_DEPTH];
    Buf bitsets[MAX_DEPTH];
    int ndepth=0;
    int cleared=0;

    u8 *current=(u8*)malloc(n);
    u8 *aligned=(u8*)malloc(n);
    memcpy(current, data, n);

    for (int depth=0; depth<MAX_DEPTH; depth++) {
        /* Sweep only uncleared bits */
        u64 counts[8]={0};
        for (size_t i=0;i<n;i++) {
            u8 v=current[i];
            for (int b=0;b<8;b++) if (!((cleared>>b)&1) && (v>>b)&1) counts[b]++;
        }
        int best=-1; u64 best_c=UINT64_MAX;
        for (int b=0;b<8;b++) {
            if (!((cleared>>b)&1) && counts[b]<best_c) { best_c=counts[b]; best=b; }
        }
        if (best<0) break;

        Buf fp = bit_clear(current, n, best, aligned);
        size_t fk = fp.n/sizeof(u32);
        bitsets[ndepth] = build_bitset((u32*)fp.d, fk, n);
        buf_free(&fp);
        which_bits[ndepth]=(u8)best;
        cleared |= (1<<best);
        ndepth++;

        memcpy(current, aligned, n);
        if (is_all_zero(current,n) || is_all_one(current,n)) break;
    }
    free(aligned);

    /* Concatenate bitsets */
    size_t bsz = (n+7)/8;
    Buf flags_cat = buf_new((size_t)ndepth * bsz + 4);
    for (int i=0;i<ndepth;i++) {
        buf_app(&flags_cat, bitsets[i].d, bitsets[i].n);
        buf_free(&bitsets[i]);
    }

    Buf flags_gz = term_encode(flags_cat.d, flags_cat.n);
    Buf term_gz  = term_encode(current, n);
    buf_free(&flags_cat); free(current);

    Buf out = buf_new(1 + ndepth + 4 + flags_gz.n + 4 + term_gz.n + 4);
    buf_push(&out,(u8)ndepth);
    buf_app(&out, which_bits, ndepth);
    u8 tmp[4]; wr32(tmp,(u32)flags_gz.n); buf_app(&out,tmp,4);
    buf_app(&out, flags_gz.d, flags_gz.n);
    wr32(tmp,(u32)term_gz.n); buf_app(&out,tmp,4);
    buf_app(&out, term_gz.d, term_gz.n);
    buf_free(&flags_gz); buf_free(&term_gz);
    return out;
}


/* ── CAIM decode ─────────────────────────────────────────────────────────── */

static Buf caim_decode(const u8 *payload, size_t plen, size_t n) {
    (void)plen;
    int n_levels = payload[0];
    const u8 *which_bits = payload+1;
    size_t pos = 1 + n_levels;
    u32 flags_len = rd32(payload+pos); pos+=4;
    const u8 *flags_gz = payload+pos; pos+=flags_len;
    u32 term_len  = rd32(payload+pos); pos+=4;
    const u8 *term_gz = payload+pos;

    size_t bsz = (n+7)/8;

    Buf flags_cat = term_decode(flags_gz, flags_len);
    Buf term_buf  = term_decode(term_gz, term_len);

    u8 *current = (u8*)malloc(n);
    memcpy(current, term_buf.d, term_buf.n < n ? term_buf.n : n);
    buf_free(&term_buf);

    /* Reconstruct bottom-up */
    for (int i=n_levels-1; i>=0; i--) {
        int bit = which_bits[i];
        const u8 *bs = flags_cat.d + (size_t)i * bsz;
        Buf fp = bitset_to_positions(bs, n);
        size_t fk = fp.n / sizeof(u32);
        reconstruct(current, (u32*)fp.d, fk, bit);
        buf_free(&fp);
    }
    buf_free(&flags_cat);

    Buf out; out.d=current; out.n=n; out.cap=n;
    return out;
}


/* ── Container encode/decode ─────────────────────────────────────────────── */

static Buf aim_encode(const u8 *data, size_t n) {
    u8 digest[32]; sha256(data, n, digest);

    Buf pr = recursive_encode(data, n);
    Buf pc = caim_encode(data, n);

    u8 mode;
    Buf *best;
    if (pr.n <= pc.n) { mode=MODE_RECURSIVE; best=&pr; buf_free(&pc); }
    else              { mode=MODE_CAIM;      best=&pc; buf_free(&pr); }

    Buf out = buf_new(HEADER_SIZE + best->n);
    buf_app(&out, AIM_MAGIC, 4);
    buf_push(&out, mode);
    u8 tmp[8]; wr64(tmp,(u64)n); buf_app(&out,tmp,8);
    buf_app(&out, digest, 32);
    buf_app(&out, best->d, best->n);
    buf_free(best);
    return out;
}

typedef struct { Buf data; int ok; char err[128]; } AimResult;

static AimResult aim_decode(const u8 *container, size_t clen, int verify) {
    AimResult r; r.ok=0; r.err[0]=0;
    if (clen < HEADER_SIZE || memcmp(container, AIM_MAGIC, 4)!=0) {
        snprintf(r.err, sizeof(r.err), "Not an AIM4 file (bad magic).");
        r.data.d=NULL; r.data.n=r.data.cap=0; return r;
    }
    u8 mode = container[4];
    u64 orig_n = rd64(container+5);
    const u8 *sha_expected = container+13;
    const u8 *payload = container+HEADER_SIZE;
    size_t   paylen   = clen - HEADER_SIZE;

    if (mode==MODE_RECURSIVE) r.data = recursive_decode(payload, paylen, (size_t)orig_n);
    else if (mode==MODE_CAIM) r.data = caim_decode(payload, paylen, (size_t)orig_n);
    else { snprintf(r.err,sizeof(r.err),"Unknown mode %d",mode); return r; }

    if (verify) {
        u8 actual[32]; sha256(r.data.d, r.data.n, actual);
        if (memcmp(actual, sha_expected, 32)!=0) {
            snprintf(r.err,sizeof(r.err),"SHA-256 mismatch.");
            buf_free(&r.data); return r;
        }
    }
    r.ok=1; return r;
}


/* ── Bench ───────────────────────────────────────────────────────────────── */

static double now_s() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

static void bench(const u8 *data, size_t n, const char *filename) {
    /* "raw H0" = order-0 Huffman (matches Python bench baseline) */
    Buf raw_h = huffman_encode(data, n);
    size_t raw_gz = raw_h.n; buf_free(&raw_h);

    printf("\nAIM v16 Benchmark  --  %zu bytes  (%.2f MiB)\n", n, (double)n/(1024*1024));
    printf("Input file  : %s\n", filename);
    printf("Raw H0      : %zu bytes  (100.00%%)\n\n", raw_gz);
    printf("%-14s  %12s  %10s  %12s  %8s  %9s  %7s  %3s\n",
           "mode","payload","+ header","total","ratio","vs H0","time","ok");
    printf("%s\n", "────────────────────────────────────────────────────────────────────────────────────");

    const char *names[] = {"recursive","caim"};
    for (int m=0; m<2; m++) {
        double t0 = now_s();
        Buf pl;
        if (m==0) pl = recursive_encode(data, n);
        else      pl = caim_encode(data, n);
        double el = now_s() - t0;

        size_t total = pl.n + HEADER_SIZE;
        double ratio = 100.0 * total / n;
        double vs    = 100.0 * ((double)total - raw_gz) / n;

        /* Verify roundtrip */
        u8 digest[32]; sha256(data, n, digest);
        u8 hdr[HEADER_SIZE];
        memcpy(hdr, AIM_MAGIC, 4); hdr[4]=(u8)m;
        wr64(hdr+5,(u64)n); memcpy(hdr+13,digest,32);
        Buf container = buf_new(HEADER_SIZE + pl.n);
        buf_app(&container,hdr,HEADER_SIZE);
        buf_app(&container,pl.d,pl.n);
        buf_free(&pl);
        AimResult res = aim_decode(container.d, container.n, 1);
        buf_free(&container);
        const char *ok_str = (res.ok && res.data.n==n && memcmp(res.data.d,data,n)==0) ? "✓" : "✗";
        if (res.ok) buf_free(&res.data);

        printf("%-14s  %12zu  %10u  %12zu  %7.2f%%  %+8.2f%%  [%.1fs]  %s\n",
               names[m], total-HEADER_SIZE, HEADER_SIZE, total, ratio, vs, el, ok_str);
    }
    printf("\n");

    /* Full encode (auto mode) */
    double t0 = now_s();
    Buf c = aim_encode(data, n);
    double el = now_s() - t0;
    size_t total = c.n;
    const char *mode_name = c.d[4]==MODE_RECURSIVE ? "recursive" : "caim";
    double ratio = 100.0*total/n;
    double vs = 100.0*((double)total - raw_gz)/n;
    printf("%-14s  %12s  %10s  %12zu  %7.2f%%  %+8.2f%%  [%.1fs]  mode=%s\n",
           "auto (winner)","—","—",total,ratio,vs,el,mode_name);
    buf_free(&c);
    printf("\n");
}


/* ── CLI main ────────────────────────────────────────────────────────────── */

static u8 *read_file(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END); size_t n=(size_t)ftell(f); reseek: fseek(f,0,SEEK_SET);
    u8 *buf = (u8*)malloc(n+1);
    size_t got = fread(buf, 1, n, f); fclose(f);
    (void)got; *out_n=n; return buf;
    goto reseek; /* suppress warning */
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "AIM v16 — Adaptive Isolating Model Compression\n"
            "Usage:\n"
            "  aim encode <input> <output>\n"
            "  aim decode <input> <output> [--no-verify]\n"
            "  aim bench  <file>\n");
        return 1;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd,"encode")==0 && argc>=4) {
        size_t n; u8 *data = read_file(argv[2], &n);
        if (!data) return 1;
        double t0=now_s();
        Buf c = aim_encode(data, n);
        double el=now_s()-t0;

        FILE *f = fopen(argv[3],"wb");
        if (!f) { fprintf(stderr,"Cannot write '%s'\n",argv[3]); return 1; }
        fwrite(c.d, 1, c.n, f); fclose(f);

        Buf raw_h = huffman_encode(data, n);
        size_t raw_gz=raw_h.n; buf_free(&raw_h);
        const char *mode_name = c.d[4]==MODE_RECURSIVE?"recursive":"caim";
        printf("Encoded  '%s'  (%zu bytes)\n", argv[2], n);
        printf("  Mode     : %s\n", mode_name);
        printf("  Output   : %zu bytes  (%.2f%%)\n", c.n, 100.0*c.n/n);
        printf("  Raw H0   : %zu bytes  (%.2f%%)\n", raw_gz, 100.0*raw_gz/n);
        printf("  Delta    : %+zd bytes  (%+.2f%%)\n", (ssize_t)(c.n-raw_gz), 100.0*((double)c.n-raw_gz)/n);
        printf("  Written  : '%s'  [%.2fs]\n", argv[3], el);
        buf_free(&c); free(data);

    } else if (strcmp(cmd,"decode")==0 && argc>=4) {
        /* decode <input> <output> [--no-verify]
           argv[2]=input  argv[3]=output  argv[4]=optional flag */
        int verify = 1;
        if (argc>=5 && strcmp(argv[4],"--no-verify")==0) verify=0;
        size_t cn; u8 *container = read_file(argv[2], &cn);
        if (!container) return 1;
        double t0=now_s();
        AimResult res = aim_decode(container, cn, verify);
        double el=now_s()-t0;
        free(container);
        if (!res.ok) { fprintf(stderr,"Decode error: %s\n",res.err); return 1; }
        FILE *f = fopen(argv[3],"wb");
        if (!f) { fprintf(stderr,"Cannot write '%s'\n",argv[3]); return 1; }
        fwrite(res.data.d, 1, res.data.n, f); fclose(f);
        printf("Decoded '%s'  ->  %zu bytes  %s  [%.2fs]\n",
               argv[2], res.data.n, verify ? "verified" : "(unverified)", el);
        buf_free(&res.data);

    } else if (strcmp(cmd,"bench")==0 && argc>=3) {
        size_t n; u8 *data = read_file(argv[2], &n);
        if (!data) return 1;
        bench(data, n, argv[2]);
        free(data);
    } else {
        fprintf(stderr,"Unknown command '%s'\n", cmd);
        return 1;
    }
    return 0;
}

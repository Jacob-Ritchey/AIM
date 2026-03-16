#include "bench.h"
#include "aim3.h"
#include "bit_utils.h"
#include "flags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "portable_time.h"
#include <zlib.h>

/* ── EF theoretical bits: k*l + (N>>l) + k ─────────────────────────────── */

static size_t ef_theoretical_bits(size_t k, size_t N)
{
    if (k == 0) return 0;
    int l = 0;
    if (N > k) {
        double ratio = (double)N / (double)k;
        l = (int)floor(log2(ratio));
        if (l < 0) l = 0;
    }
    return (size_t)k * (size_t)l + (N >> l) + k;
}

/* ── RLE theoretical bits ───────────────────────────────────────────────── */

static size_t rle_theoretical_bits(const uint8_t *flag_bs, size_t flag_bs_len,
                                   size_t N)
{
    if (N == 0) return 8;
    size_t bits = 8; /* first_bit byte */
    int    cur  = (int)(flag_bs[0] & 1);
    size_t run  = 0;
    for (size_t i = 0; i < N; i++) {
        int b = (int)((flag_bs[i >> 3] >> (i & 7)) & 1);
        if (b == cur) { run++; }
        else {
            int k = 0; size_t t = run; while (t > 1) { t >>= 1; k++; }
            bits += (size_t)(2*k + 1);
            cur = b; run = 1;
        }
    }
    if (run > 0) {
        int k = 0; size_t t = run; while (t > 1) { t >>= 1; k++; }
        bits += (size_t)(2*k + 1);
    }
    (void)flag_bs_len;
    return bits;
}

/* ── gz helper for benchmark ─────────────────────────────────────────────── */

static size_t gz_size(const uint8_t *src, size_t len, int level)
{
    z_stream zs = {0};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;

    const size_t IN_CHUNK  = 1u << 30;
    const size_t OUT_CHUNK = 65536;
    uint8_t tmp[65536];
    const uint8_t *p = src;
    size_t remaining = len, total_out = 0;

    do {
        uInt this_in = (uInt)(remaining < IN_CHUNK ? remaining : IN_CHUNK);
        zs.next_in  = (Bytef *)p;
        zs.avail_in = this_in;
        p += this_in; remaining -= this_in;
        int flush = (remaining == 0) ? Z_FINISH : Z_NO_FLUSH;
        int zret;
        do {
            zs.next_out  = (Bytef *)tmp;
            zs.avail_out = (uInt)OUT_CHUNK;
            zret = deflate(&zs, flush);
            if (zret == Z_STREAM_ERROR) { deflateEnd(&zs); return 0; }
            total_out += OUT_CHUNK - zs.avail_out;
        } while (zs.avail_out == 0);
        if (zret == Z_STREAM_END) break;
    } while (remaining > 0);

    deflateEnd(&zs);
    return total_out;
}

/* ── flag_bench ──────────────────────────────────────────────────────────── */

void flag_bench(const uint8_t *data, size_t n)
{
    printf("\nFlag encoding comparison — %zu bytes\n", n);
    printf("%4s  %10s  %8s  %10s  %10s  %10s  %10s  %11s  %10s\n",
           "bit","k flags","density","gz bytes","EF bytes","RLE bytes",
           "EF theory","RLE theory","winner");
    printf("%s\n", "──────────────────────────────────────────────────────────────────────────────────────────────────");

    size_t flag_bs_len = (n + 7) / 8;
    for (int bit = 0; bit < 8; bit++) {
        Buf     aligned; buf_init(&aligned);
        uint8_t *flag_bs = calloc(1, flag_bs_len);
        if (!flag_bs) continue;
        if (bit_clear(data, n, bit, &aligned, flag_bs) < 0) {
            buf_free(&aligned); free(flag_bs); continue;
        }
        buf_free(&aligned);

        /* Count set bits. */
        size_t k = 0;
        for (size_t i = 0; i < flag_bs_len; i++) {
            uint8_t b = flag_bs[i]; while (b) { k++; b &= b - 1; }
        }
        double density = n ? (double)k / (double)n : 0.0;

        Buf b_gz, b_ef, b_rle;
        buf_init(&b_gz); buf_init(&b_ef); buf_init(&b_rle);
        int m_gz, m_ef, m_rle;
        encode_flags_gz( flag_bs, flag_bs_len, (uint64_t)n, AIM3_GZ_LEVEL, &b_gz, &m_gz);
        encode_flags_ef( flag_bs, flag_bs_len, (uint64_t)n, &b_ef, &m_ef);
        encode_flags_rle(flag_bs, flag_bs_len, (uint64_t)n, &b_rle, &m_rle);

        size_t ef_th  = (k > 0) ? (ef_theoretical_bits(k, n) + 7) / 8 : 0;
        size_t rle_th = (rle_theoretical_bits(flag_bs, flag_bs_len, n) + 7) / 8;

        const char *winner = "gz";
        if (b_ef.len < b_gz.len && b_ef.len <= b_rle.len) winner = "EF";
        else if (b_rle.len < b_gz.len)                    winner = "RLE";

        printf("%4d  %10zu  %7.1f%%  %10zu  %10zu  %10zu  %10zu  %11zu  %10s\n",
               bit, k, 100.0*density,
               b_gz.len, b_ef.len, b_rle.len,
               ef_th, rle_th, winner);

        buf_free(&b_gz); buf_free(&b_ef); buf_free(&b_rle);
        free(flag_bs);
    }
}

/* ── benchmark ───────────────────────────────────────────────────────────── */

static double clock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void benchmark(const uint8_t *data, size_t n, int fast)
{
    size_t raw_gz = gz_size(data, n, AIM3_GZ_LEVEL);

    printf("\nFile size    : %zu bytes\n", n);
    printf("Raw gzip     : %zu bytes  (%.2f%%)\n", raw_gz,
           n ? 100.0*raw_gz/n : 0.0);

    static const struct { const char *label; int backend; } configs[] = {
        {"auto",       -1},
        {"gzip",        0},
        {"ans0",        1},
        {"ans1",        2},
        {"ans2d",       4},
        {"ans-stride",  5},
    };
    int n_configs = fast ? 3 : 6;

    printf("\n%-14s  %4s  %12s  %7s  %10s  %12s  %10s  %7s  %9s\n",
           "config","bit","flag mode","backend","flag","aln","total","ratio","vs_gz");
    printf("%s\n", "────────────────────────────────────────────────────────────────────────────────────────────────────────");

    static const char *flag_names[] = {"gap+gz","bitset+gz","Elias-Fano","Gamma-RLE"};
    static const char *be_names[]   = {"gzip","ANS-0","ANS-1","?","ANS-2d","ANS-stride"};

    for (int ci = 0; ci < n_configs; ci++) {
        Buf out; buf_init(&out);
        double t0 = clock_now();
        int r = aim3_encode(data, n,
                            /*target_bit=*/-1, configs[ci].backend,
                            AIM3_GZ_LEVEL, /*verbose=*/0, /*sample_cap=*/0,
                            /*src_path=*/NULL,
                            &out);
        double elapsed = clock_now() - t0;

        if (r < 0 || out.len < AIM3_HEADER_SIZE + 4) {
            printf("%-14s  encode failed\n", configs[ci].label);
            buf_free(&out); continue;
        }

        uint8_t tb  = out.data[5];
        uint8_t fm  = out.data[6];
        uint8_t be  = out.data[7];
        uint32_t fl = ((uint32_t)out.data[16]<<24)|((uint32_t)out.data[17]<<16)|
                      ((uint32_t)out.data[18]<<8)|(uint32_t)out.data[19];
        uint32_t al = ((uint32_t)out.data[20]<<24)|((uint32_t)out.data[21]<<16)|
                      ((uint32_t)out.data[22]<<8)|(uint32_t)out.data[23];
        size_t total = out.len;
        long long delta = (long long)total - (long long)raw_gz;

        const char *fn = (fm < 4) ? flag_names[fm] : "?";
        const char *bn = (be < 6) ? be_names[be]   : "?";

        printf("%-14s  %4u  %12s  %7s  %10u  %12u  %10zu  %6.2f%%  %+8.2f%%  [%.1fs]\n",
               configs[ci].label, tb, fn, bn, fl, al, total,
               n ? 100.0*total/n : 0.0,
               n ? 100.0*delta/n : 0.0,
               elapsed);

        buf_free(&out);
    }
}

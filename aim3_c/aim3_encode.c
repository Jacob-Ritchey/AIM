#include "aim3.h"
#include "bit_utils.h"
#include "flags.h"
#include "ans.h"
#include "recursive.h"
#include "sha256.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "portable_time.h"
#include <zlib.h>

const char *aim3_errmsg = "ok";

/* ── file-static helpers ────────────────────────────────────────────────── */

static double clock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static AIM3EncodeStats s_enc_stats;

const AIM3EncodeStats *aim3_encode_stats(void) { return &s_enc_stats; }

/* ── gzip helper ────────────────────────────────────────────────────────── */

static int gz_compress_buf(const uint8_t *src, size_t src_len, int level, Buf *out)
{
    z_stream zs = {0};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;

    /* Feed input in ≤2 GB chunks; avail_in is uInt (32-bit). */
    const size_t IN_CHUNK  = 1u << 30;  /* 1 GB */
    const size_t OUT_CHUNK = 65536;
    const uint8_t *p = src;
    size_t remaining = src_len;
    int zret;

    do {
        uInt this_in = (uInt)(remaining < IN_CHUNK ? remaining : IN_CHUNK);
        zs.next_in  = (Bytef *)p;
        zs.avail_in = this_in;
        p         += this_in;
        remaining -= this_in;
        int flush = (remaining == 0) ? Z_FINISH : Z_NO_FLUSH;

        do {
            if (buf_reserve(out, out->len + OUT_CHUNK) < 0) {
                deflateEnd(&zs); return -1;
            }
            zs.next_out  = out->data + out->len;
            zs.avail_out = (uInt)OUT_CHUNK;
            zret = deflate(&zs, flush);
            if (zret == Z_STREAM_ERROR) { deflateEnd(&zs); return -1; }
            out->len += OUT_CHUNK - zs.avail_out;
        } while (zs.avail_out == 0);

    } while (remaining > 0);

    deflateEnd(&zs);
    return (zret == Z_STREAM_END) ? 0 : -1;
}

/* ── select_global_bit ──────────────────────────────────────────────────── */

static int select_global_bit(const uint8_t *data, size_t n, size_t sample_cap,
                              int gz_level, int verbose)
{
    const uint8_t *sample = data;
    size_t         sn     = n;
    if (sample_cap > 0 && sample_cap < n) sn = sample_cap;

    int    best_b    = 0;
    size_t best_cost = (size_t)-1;

    size_t s_flag_bs_len = (sn + 7) / 8;
    for (int b = 0; b < 8; b++) {
        Buf     aligned; buf_init(&aligned);
        uint8_t *flag_bs = calloc(1, s_flag_bs_len);
        if (!flag_bs) continue;

        if (bit_clear(sample, sn, b, &aligned, flag_bs) < 0) {
            buf_free(&aligned); free(flag_bs); continue;
        }

        Buf flags_buf; buf_init(&flags_buf);
        int mode;
        if (encode_flags_best(flag_bs, s_flag_bs_len, (uint64_t)sn,
                              gz_level, &flags_buf, &mode) < 0) {
            buf_free(&aligned); free(flag_bs); buf_free(&flags_buf); continue;
        }

        Buf gz_aligned; buf_init(&gz_aligned);
        gz_compress_buf(aligned.data, aligned.len, gz_level, &gz_aligned);

        size_t cost = flags_buf.len + gz_aligned.len;
        if (verbose)
            printf("    bit %d: cost=%zu bytes\n", b, cost);
        if (cost < best_cost) { best_cost = cost; best_b = b; }

        buf_free(&aligned); free(flag_bs);
        buf_free(&flags_buf); buf_free(&gz_aligned);
    }
    return best_b;
}

/* ── select_global_bit_stream ────────────────────────────────────────────── */

/* Streaming equivalent of select_global_bit: scans the file once with a
 * popcount per bit-plane and returns the sparsest bit.  Uses no N-byte input
 * buffer; peak memory = 64 KB stack chunk.                                    */
static int select_global_bit_stream(const char *path, size_t n,
                                    size_t sample_cap, int verbose)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    size_t limit = (sample_cap > 0 && sample_cap < n) ? sample_cap : n;
    uint64_t cnt[8] = {0};
    uint8_t  chunk[65536];
    size_t   total = 0, got;

    while (total < limit &&
           (got = fread(chunk, 1,
                        (sizeof(chunk) < limit - total ? sizeof(chunk)
                                                       : limit - total),
                        f)) > 0) {
        for (size_t i = 0; i < got; i++)
            for (int b = 0; b < 8; b++)
                if (chunk[i] & (1u << b)) cnt[b]++;
        total += got;
    }
    fclose(f);

    int      best_b   = 0;
    uint64_t best_cnt = cnt[0];
    for (int b = 1; b < 8; b++)
        if (cnt[b] < best_cnt) { best_cnt = cnt[b]; best_b = b; }

    if (verbose)
        for (int b = 0; b < 8; b++)
            printf("    bit %d: set_count=%" PRIu64 "\n", b, cnt[b]);

    return best_b;
}

/* ── aim3_encode ────────────────────────────────────────────────────────── */

int aim3_encode(const uint8_t *data, size_t n,
                int target_bit, int backend,
                int gz_level, int verbose, size_t sample_cap,
                const char *src_path,
                Buf *out)
{
    aim3_errmsg = "ok";
    memset(&s_enc_stats, 0, sizeof(s_enc_stats));
    s_enc_stats.input_bytes = n;
    double t_total = clock_now();

    /* 1. SHA-256 of original data (streaming when src_path provided). */
    uint8_t sha256[SHA256_BLOCK_SIZE];
    if (src_path) {
        if (sha256_file(src_path, sha256) < 0) {
            aim3_errmsg = "sha256_file failed"; return -1;
        }
    } else {
        sha256_buf(data, n, sha256);
    }

    /* 2. Select target bit (streaming popcount when src_path provided). */
    double t_bit = clock_now();
    if (target_bit < 0) {
        if (src_path)
            target_bit = select_global_bit_stream(src_path, n, sample_cap, verbose);
        else
            target_bit = select_global_bit(data, n, sample_cap, gz_level, verbose);
    }
    double t_bit_elapsed = clock_now() - t_bit;
    s_enc_stats.selected_bit      = target_bit;
    s_enc_stats.bit_select_time_s = t_bit_elapsed;
    printf("  Bit plane  : bit %d  [%.2fs]\n", target_bit, t_bit_elapsed);
    fflush(stdout);

    /* 3. Strip target bit (streaming from file when src_path provided). */
    Buf      aligned; buf_init(&aligned);
    size_t   flag_bs_len = ((size_t)n + 7) / 8;
    uint8_t *flag_bs = calloc(1, flag_bs_len);
    if (!flag_bs) { aim3_errmsg = "flag_bs OOM"; return -1; }
    int bc_ret = src_path
        ? bit_clear_stream(src_path, n, target_bit, &aligned, flag_bs)
        : bit_clear(data, n, target_bit, &aligned, flag_bs);
    if (bc_ret < 0) {
        free(flag_bs); aim3_errmsg = "bit_clear OOM"; return -1;
    }

    /* 4. Encode flag stream (best of 3). */
    double t_flag = clock_now();
    Buf flag_block; buf_init(&flag_block);
    int flag_mode;
    if (encode_flags_best(flag_bs, flag_bs_len, (uint64_t)n,
                          gz_level, &flag_block, &flag_mode) < 0) {
        aim3_errmsg = "encode_flags OOM";
        buf_free(&aligned); free(flag_bs); return -1;
    }
    double t_flag_elapsed = clock_now() - t_flag;
    free(flag_bs);
    s_enc_stats.flag_mode  = flag_mode;
    s_enc_stats.flag_bytes = flag_block.len;
    s_enc_stats.flag_time_s = t_flag_elapsed;
    static const char *flag_names[] = {"gap+gz","bitset+gz","Elias-Fano","Gamma-RLE"};
    printf("  Flags      : %s \xe2\x86\x92 %zu bytes  [%.2fs]\n",
           flag_mode < 4 ? flag_names[flag_mode] : "?",
           flag_block.len, t_flag_elapsed);
    fflush(stdout);

    /* 5. Remap aligned bytes to [0,127]. */
    Buf syms; buf_init(&syms);
    if (remap_to_128(aligned.data, aligned.len, target_bit, &syms) < 0) {
        aim3_errmsg = "remap OOM";
        buf_free(&aligned); buf_free(&flag_block); return -1;
    }

    /* 6. Try backends, pick smallest. */
    int try_list[5]; int n_try = 0;
    if (backend == -1) {
        try_list[0]=0; try_list[1]=1; try_list[2]=2;
        try_list[3]=4; try_list[4]=5; n_try=5;
    } else {
        try_list[0]=backend; n_try=1;
    }

    Buf best_aln; buf_init(&best_aln);
    int best_be = try_list[0];

    double t_be = clock_now();
    for (int ti = 0; ti < n_try; ti++) {
        int be = try_list[ti];
        Buf aln; buf_init(&aln);
        /* Pre-size to input length — output can't exceed this, prevents doubling. */
        if (buf_reserve_exact(&aln, syms.len) < 0) { continue; }
        aln.len = 0;
        int r = 0;

        if      (be == 0) r = gz_compress_buf(aligned.data, aligned.len, gz_level, &aln);
        else if (be == 1) r = ans0_encode(syms.data, syms.len, &aln);
        else if (be == 2) r = ans1_encode(syms.data, syms.len, &aln);
        else if (be == 4) r = ans2_diff_encode(syms.data, syms.len, &aln);
        else if (be == 5) r = ans_stride_encode(syms.data, syms.len, &aln);

        if (r < 0) { buf_free(&aln); continue; }

        size_t aln_len = aln.len;
        if (be < 6) s_enc_stats.backend_size[be] = aln_len;

        if (best_aln.len == 0 || aln.len < best_aln.len) {
            buf_free(&best_aln);
            best_aln = aln;
            best_be  = be;
        } else {
            buf_free(&aln);
        }

        if (verbose) {
            static const char *be_names[] = {"gzip","ANS-0","ANS-1","?","ANS-2d","ANS-stride"};
            const char *name = (be < 6) ? be_names[be] : "?";
            printf("  %-11s: %zu bytes%s\n", name, aln_len,
                   aln_len == best_aln.len ? " \xe2\x86\x90" : "");
        }
    }
    double t_be_elapsed = clock_now() - t_be;

    s_enc_stats.best_backend  = best_be;
    s_enc_stats.backend_time_s = t_be_elapsed;

    /* Extract stride k from first byte of stride payload. */
    if (best_be == BACKEND_ANS_STRIDE && best_aln.len > 0)
        s_enc_stats.stride_k = best_aln.data[0];

    /* Print compact "Backends" summary line. */
    {
        static const char *be_short[] = {"gzip","ans-0","ans-1","?","ans-2d","ans-stride"};
        printf("  Backends   :");
        for (int ti = 0; ti < n_try; ti++) {
            int be = try_list[ti];
            if (be < 0 || be >= 6 || s_enc_stats.backend_size[be] == 0) continue;
            size_t sz = s_enc_stats.backend_size[be];
            if (sz >= 1024*1024)
                printf(" %s=%.1fM", be_short[be], sz / (1024.0*1024.0));
            else if (sz >= 1024)
                printf(" %s=%.1fK", be_short[be], sz / 1024.0);
            else
                printf(" %s=%zu", be_short[be], sz);
            if (be == best_be) {
                if (best_be == BACKEND_ANS_STRIDE)
                    printf(" \xe2\x86\x90  stride k=%u", (unsigned)s_enc_stats.stride_k);
                else
                    printf(" \xe2\x86\x90");
            }
        }
        printf("  [%.1fs]\n", t_be_elapsed);
        fflush(stdout);
    }

    buf_free(&aligned);
    buf_free(&syms);

    if (best_aln.len == 0 && n > 0) {
        aim3_errmsg = "all backends failed";
        buf_free(&flag_block); return -1;
    }

    /* 7. Build single-pass container (v0x09). */
    uint8_t hdr[AIM3_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    hdr[0]='A'; hdr[1]='I'; hdr[2]='M'; hdr[3]='3';
    hdr[4]  = AIM3_VERSION;
    hdr[5]  = (uint8_t)(target_bit & 0xFF);
    hdr[6]  = (uint8_t)(flag_mode  & 0xFF);
    hdr[7]  = (uint8_t)(best_be    & 0xFF);
    uint64_t os = (uint64_t)n;
    hdr[8] =(uint8_t)(os>>56); hdr[9] =(uint8_t)(os>>48);
    hdr[10]=(uint8_t)(os>>40); hdr[11]=(uint8_t)(os>>32);
    hdr[12]=(uint8_t)(os>>24); hdr[13]=(uint8_t)(os>>16);
    hdr[14]=(uint8_t)(os>> 8); hdr[15]=(uint8_t)os;
    uint64_t fl_len = (uint64_t)flag_block.len;
    hdr[16]=(uint8_t)(fl_len>>56); hdr[17]=(uint8_t)(fl_len>>48);
    hdr[18]=(uint8_t)(fl_len>>40); hdr[19]=(uint8_t)(fl_len>>32);
    hdr[20]=(uint8_t)(fl_len>>24); hdr[21]=(uint8_t)(fl_len>>16);
    hdr[22]=(uint8_t)(fl_len>> 8); hdr[23]=(uint8_t)fl_len;
    uint64_t al_len = (uint64_t)best_aln.len;
    hdr[24]=(uint8_t)(al_len>>56); hdr[25]=(uint8_t)(al_len>>48);
    hdr[26]=(uint8_t)(al_len>>40); hdr[27]=(uint8_t)(al_len>>32);
    hdr[28]=(uint8_t)(al_len>>24); hdr[29]=(uint8_t)(al_len>>16);
    hdr[30]=(uint8_t)(al_len>> 8); hdr[31]=(uint8_t)al_len;
    memcpy(hdr + 32, sha256, SHA256_BLOCK_SIZE);

    Buf sp_buf; buf_init(&sp_buf);
    int ret = 0;
    {
        size_t sp_total = (size_t)AIM3_HEADER_SIZE + flag_block.len + best_aln.len;
        if (buf_reserve_exact(&sp_buf, sp_total) < 0) ret = -1;
    }
    if (ret == 0 && buf_append(&sp_buf, hdr,              AIM3_HEADER_SIZE) < 0) ret = -1;
    if (ret == 0 && buf_append(&sp_buf, flag_block.data, flag_block.len) < 0) ret = -1;
    if (ret == 0 && buf_append(&sp_buf, best_aln.data,  best_aln.len)   < 0) ret = -1;
    buf_free(&flag_block);
    buf_free(&best_aln);
    if (ret < 0) { buf_free(&sp_buf); aim3_errmsg = "output OOM"; return -1; }

    /* 8. In auto mode, also try recursive multi-pass (v0x0A) and keep smaller. */
    Buf winner; buf_init(&winner);
    int used_recursive = 0;

    double t_rec = clock_now();
    if (backend == -1 && n >= 8) {
        Buf rec_payload; buf_init(&rec_payload);
        /* Pre-size rec_payload to sp_buf.len as a reasonable upper bound — recursive
         * output is unlikely to exceed the single-pass size, avoiding doubling. */
        (void)buf_reserve_exact(&rec_payload, sp_buf.len);
        int n_layers = 0, outer_fm = 0;
        int rec_r = src_path
            ? recursive_encode_stream(src_path, n, &rec_payload, &n_layers, &outer_fm)
            : recursive_encode(data, n, &rec_payload, &n_layers, &outer_fm);
        if (rec_r == 0
            && rec_payload.len > 0) {

            uint8_t rhdr[AIM3_HEADER_SIZE];
            memset(rhdr, 0, sizeof(rhdr));
            rhdr[0]='A'; rhdr[1]='I'; rhdr[2]='M'; rhdr[3]='3';
            rhdr[4] = AIM3_VERSION_RECURSIVE;
            rhdr[5] = (uint8_t)(n_layers & 0xFF);
            rhdr[6] = (uint8_t)(outer_fm & 0xFF);
            rhdr[7] = (uint8_t)BACKEND_ANS_STRIDE;
            rhdr[8] =(uint8_t)(os>>56); rhdr[9] =(uint8_t)(os>>48);
            rhdr[10]=(uint8_t)(os>>40); rhdr[11]=(uint8_t)(os>>32);
            rhdr[12]=(uint8_t)(os>>24); rhdr[13]=(uint8_t)(os>>16);
            rhdr[14]=(uint8_t)(os>> 8); rhdr[15]=(uint8_t)os;
            uint32_t rl = (uint32_t)rec_payload.len;
            rhdr[20]=(uint8_t)(rl>>24); rhdr[21]=(uint8_t)(rl>>16);
            rhdr[22]=(uint8_t)(rl>> 8); rhdr[23]=(uint8_t)rl;
            memcpy(rhdr + 24, sha256, SHA256_BLOCK_SIZE);

            Buf rec_buf; buf_init(&rec_buf);
            (void)buf_reserve_exact(&rec_buf, AIM3_HEADER_SIZE + rec_payload.len);
            if (buf_append(&rec_buf, rhdr,             AIM3_HEADER_SIZE) == 0 &&
                buf_append(&rec_buf, rec_payload.data, rec_payload.len)  == 0) {
                if (rec_buf.len < sp_buf.len) {
                    winner = rec_buf;
                    used_recursive = 1;
                    buf_free(&sp_buf);
                    s_enc_stats.n_layers       = n_layers;
                    s_enc_stats.recursive_bytes = rec_buf.len;
                } else {
                    buf_free(&rec_buf);
                    winner = sp_buf;
                }
            } else {
                buf_free(&rec_buf);
                winner = sp_buf;
            }
        } else {
            winner = sp_buf;
        }
        buf_free(&rec_payload);
    } else {
        winner = sp_buf;
    }
    double t_rec_elapsed = clock_now() - t_rec;

    s_enc_stats.used_recursive   = used_recursive;
    s_enc_stats.recursive_time_s = t_rec_elapsed;

    if (backend == -1 && n >= 8) {
        if (used_recursive) {
            printf("  Recursive  : %d layers \xe2\x86\x92 %zu bytes \xe2\x86\x90 winner  [%.1fs]\n",
                   s_enc_stats.n_layers, s_enc_stats.recursive_bytes, t_rec_elapsed);
        } else {
            printf("  Recursive  : skipped (single-pass smaller)  [%.1fs]\n",
                   t_rec_elapsed);
        }
        fflush(stdout);
    }

    /* 9. Final stats and output summary. */
    s_enc_stats.container_version = winner.len > 4 ? winner.data[4] : AIM3_VERSION;
    s_enc_stats.output_bytes      = winner.len;
    s_enc_stats.total_time_s      = clock_now() - t_total;

    double ratio = n ? 100.0 * winner.len / n : 0.0;
    printf("  Output     : %zu bytes  %.2f%%  [%.1fs total]\n",
           winner.len, ratio, s_enc_stats.total_time_s);
    fflush(stdout);

    ret = buf_append(out, winner.data, winner.len);
    buf_free(&winner);
    if (ret < 0) aim3_errmsg = "output OOM";
    return ret;
}

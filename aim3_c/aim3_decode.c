#include "aim3.h"
#include "bit_utils.h"
#include "flags.h"
#include "ans.h"
#include "recursive.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "portable_time.h"
#include <zlib.h>

static double clock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static AIM3DecodeStats s_dec_stats;

const AIM3DecodeStats *aim3_decode_stats(void) { return &s_dec_stats; }

/* ── gzip decompression ─────────────────────────────────────────────────── */

static int gz_decompress_buf(const uint8_t *src, size_t src_len, Buf *out)
{
    z_stream zs = {0};
    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    if (inflateInit2(&zs, 15+16) != Z_OK) return -1;

    size_t chunk = 65536; int zret;
    do {
        if (buf_reserve(out, out->len + chunk) < 0) { inflateEnd(&zs); return -1; }
        zs.next_out  = out->data + out->len;
        zs.avail_out = (uInt)chunk;
        zret = inflate(&zs, Z_NO_FLUSH);
        if (zret == Z_STREAM_ERROR || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR) {
            inflateEnd(&zs); return -1;
        }
        out->len += chunk - zs.avail_out;
    } while (zret != Z_STREAM_END);

    inflateEnd(&zs);
    return 0;
}

/* ── shared decode body ─────────────────────────────────────────────────── */

static int decode_body(const uint8_t *flag_raw, size_t fl,
                       const uint8_t *aln_block, size_t al,
                       uint64_t orig, int tb, int fm, int be,
                       int verify, const uint8_t *expected_sha,
                       Buf *out)
{
    /* Decode flags. */
    size_t   flag_bs_len = ((size_t)orig + 7) / 8;
    uint8_t *flag_bs = calloc(1, flag_bs_len);
    if (!flag_bs) { aim3_errmsg = "flag_bs OOM"; return -1; }
    if (decode_flags(flag_raw, fl, orig, fm, flag_bs, flag_bs_len) < 0) {
        aim3_errmsg = "flag decode failed"; free(flag_bs); return -1;
    }

    /* Decode aligned data. */
    Buf aligned; buf_init(&aligned);
    int ret = 0;

    if (be == BACKEND_GZIP) {
        if (gz_decompress_buf(aln_block, al, &aligned) < 0) {
            aim3_errmsg = "gzip decompress failed"; ret = -1; goto done;
        }
    } else {
        /* ANS backends decode into remapped syms, then unmap. */
        Buf syms; buf_init(&syms);
        if      (be == BACKEND_ANS0)       ret = ans0_decode(aln_block, al, (size_t)orig, &syms);
        else if (be == BACKEND_ANS1)       ret = ans1_decode(aln_block, al, (size_t)orig, &syms);
        else if (be == BACKEND_ANS2D)      ret = ans2_diff_decode(aln_block, al, (size_t)orig, &syms);
        else if (be == BACKEND_ANS_STRIDE) ret = ans_stride_decode(aln_block, al, (size_t)orig, &syms);
        else {
            aim3_errmsg = "unsupported backend"; buf_free(&syms); ret = -1; goto done;
        }
        if (ret < 0) {
            aim3_errmsg = "ANS decode failed"; buf_free(&syms); goto done;
        }
        if (unmap_from_128(syms.data, syms.len, tb, &aligned) < 0) {
            aim3_errmsg = "unmap OOM"; buf_free(&syms); ret = -1; goto done;
        }
        buf_free(&syms);
    }

    /* Reconstruct original. */
    if (buf_reserve_exact(out, orig) < 0) {
        aim3_errmsg = "output OOM"; ret = -1; goto done;
    }
    out->len = (size_t)orig;
    reconstruct(aligned.data, (size_t)orig, flag_bs, tb, out->data);

    /* SHA-256 verify. */
    if (verify) {
        uint8_t actual[SHA256_BLOCK_SIZE];
        sha256_buf(out->data, out->len, actual);
        if (memcmp(actual, expected_sha, SHA256_BLOCK_SIZE) != 0) {
            aim3_errmsg = "SHA-256 mismatch"; ret = -1; goto done;
        }
    }

done:
    buf_free(&aligned);
    free(flag_bs);
    return ret;
}

/* ── Legacy v0x02 decoder ───────────────────────────────────────────────── */

static int decode_legacy_v02(const uint8_t *container, size_t clen,
                             int verify, Buf *out)
{
    /*
     * v0x02 header (60 bytes, big-endian):
     *  [0-3]  magic 4s
     *  [4]    version B
     *  [5]    target_bit B
     *  [6]    flag_mode B
     *  [7]    backend B   (ignored for v02, always gzip)
     *  [8-15] orig_size Q
     *  [16-19]? (padding/old field)
     *  [20-23]flag_len I
     *  [24-27]aln_len  I
     *  [28-59]sha256 32s
     */
    if (clen < 60) { aim3_errmsg = "container too short (v02)"; return -1; }
    int      tb  = container[5];
    int      fm  = container[6];
    uint64_t orig = ((uint64_t)container[8]<<56)|((uint64_t)container[9]<<48)|
                    ((uint64_t)container[10]<<40)|((uint64_t)container[11]<<32)|
                    ((uint64_t)container[12]<<24)|((uint64_t)container[13]<<16)|
                    ((uint64_t)container[14]<< 8)| (uint64_t)container[15];
    uint32_t fl  = ((uint32_t)container[20]<<24)|((uint32_t)container[21]<<16)|
                   ((uint32_t)container[22]<< 8)| (uint32_t)container[23];
    uint32_t al  = ((uint32_t)container[24]<<24)|((uint32_t)container[25]<<16)|
                   ((uint32_t)container[26]<< 8)| (uint32_t)container[27];
    const uint8_t *sha = container + 28;
    size_t pos = 60;

    if (pos + fl + al > clen) {
        aim3_errmsg = "container truncated (v02)"; return -1;
    }

    /* v02 always uses gzip for aligned data. */
    const uint8_t *flag_raw  = container + pos;
    const uint8_t *aln_block = container + pos + fl;

    size_t   v02_flag_bs_len = ((size_t)orig + 7) / 8;
    uint8_t *v02_flag_bs = calloc(1, v02_flag_bs_len);
    if (!v02_flag_bs) { aim3_errmsg = "flag_bs OOM (v02)"; return -1; }
    if (decode_flags(flag_raw, fl, orig, fm, v02_flag_bs, v02_flag_bs_len) < 0) {
        aim3_errmsg = "flag decode failed (v02)"; free(v02_flag_bs); return -1;
    }

    Buf aligned; buf_init(&aligned);
    if (gz_decompress_buf(aln_block, al, &aligned) < 0) {
        aim3_errmsg = "gzip decompress failed (v02)";
        free(v02_flag_bs); buf_free(&aligned); return -1;
    }

    if (buf_reserve(out, orig) < 0) {
        aim3_errmsg = "output OOM"; free(v02_flag_bs); buf_free(&aligned); return -1;
    }
    out->len = (size_t)orig;
    reconstruct(aligned.data, (size_t)orig, v02_flag_bs, tb, out->data);
    buf_free(&aligned);
    free(v02_flag_bs);

    if (verify) {
        uint8_t actual[SHA256_BLOCK_SIZE];
        sha256_buf(out->data, out->len, actual);
        if (memcmp(actual, sha, SHA256_BLOCK_SIZE) != 0) {
            aim3_errmsg = "SHA-256 mismatch (v02)"; return -1;
        }
    }
    return 0;
}

/* ── aim3_decode ────────────────────────────────────────────────────────── */

int aim3_decode(const uint8_t *container, size_t clen,
                int verify, Buf *out)
{
    aim3_errmsg = "ok";
    memset(&s_dec_stats, 0, sizeof(s_dec_stats));
    double t_total = clock_now();
    s_dec_stats.container_bytes = clen;

    if (clen < 5) { aim3_errmsg = "container too short"; return -1; }
    if (memcmp(container, "AIM3", 4) != 0) {
        aim3_errmsg = "not an AIM3 file"; return -1;
    }

    uint8_t ver = container[4];
    static const uint8_t known_versions[] = {0x02,0x04,0x05,0x06,0x07,0x08,0x09,0x0A};
    int known = 0;
    for (size_t i = 0; i < sizeof(known_versions); i++)
        if (ver == known_versions[i]) { known = 1; break; }
    if (!known) {
        aim3_errmsg = "unsupported version"; return -1;
    }

    /* v0x02 has a different header layout. */
    if (ver == 0x02) return decode_legacy_v02(container, clen, verify, out);

    /*
     * v0x04-0x09 share the 64-byte header:
     *  [0-3]  magic
     *  [4]    version
     *  [5]    target_bit
     *  [6]    flag_mode
     *  [7]    backend
     *  [8-15] orig_size  uint64 BE
     *  [16-23]flag_len   uint64 BE
     *  [24-31]aln_len    uint64 BE
     *  [32-63]sha256
     */
    if (clen < AIM3_HEADER_SIZE) { aim3_errmsg = "container too short"; return -1; }

    int      tb   = container[5];
    int      fm   = container[6];
    int      be   = container[7];
    uint64_t orig = ((uint64_t)container[8]<<56)|((uint64_t)container[9]<<48)|
                    ((uint64_t)container[10]<<40)|((uint64_t)container[11]<<32)|
                    ((uint64_t)container[12]<<24)|((uint64_t)container[13]<<16)|
                    ((uint64_t)container[14]<< 8)| (uint64_t)container[15];
    uint64_t fl   = ((uint64_t)container[16]<<56)|((uint64_t)container[17]<<48)|
                    ((uint64_t)container[18]<<40)|((uint64_t)container[19]<<32)|
                    ((uint64_t)container[20]<<24)|((uint64_t)container[21]<<16)|
                    ((uint64_t)container[22]<< 8)| (uint64_t)container[23];
    uint64_t al   = ((uint64_t)container[24]<<56)|((uint64_t)container[25]<<48)|
                    ((uint64_t)container[26]<<40)|((uint64_t)container[27]<<32)|
                    ((uint64_t)container[28]<<24)|((uint64_t)container[29]<<16)|
                    ((uint64_t)container[30]<< 8)| (uint64_t)container[31];
    const uint8_t *sha = container + 32;

    /* Populate and print container header info. */
    s_dec_stats.container_version = ver;
    s_dec_stats.flag_mode         = fm;
    s_dec_stats.backend           = be;
    s_dec_stats.orig_bytes        = (size_t)orig;

    static const char *flag_names[] = {"gap+gz","bitset+gz","Elias-Fano","Gamma-RLE"};
    static const char *be_names[]   = {"gzip","ANS-0","ANS-1","?","ANS-2d","ANS-stride"};
    if (ver == AIM3_VERSION_RECURSIVE) {
        int nl = (int)container[5];   /* n_layers stored in target_bit field */
        s_dec_stats.n_layers = nl;
        printf("  Container  : v0x%02X  %d layer%s  %s  %s\n",
               ver, nl, nl == 1 ? "" : "s",
               fm < 4 ? flag_names[fm] : "?",
               be < 6 ? be_names[be]   : "?");
    } else {
        printf("  Container  : v0x%02X  bit=%d  %s  %s\n",
               ver, tb,
               fm < 4 ? flag_names[fm] : "?",
               be < 6 ? be_names[be]   : "?");
    }
    fflush(stdout);

    size_t pos = AIM3_HEADER_SIZE;
    if (pos + fl + al > clen) {
        aim3_errmsg = "container truncated"; return -1;
    }

    int ret;

    /* v0x0A: recursive multi-pass — bypass decode_body, call recursive_decode directly. */
    if (ver == AIM3_VERSION_RECURSIVE) {
        const uint8_t *rec_payload = container + pos + fl;
        ret = recursive_decode(rec_payload, al, orig, out);
        if (ret < 0) { aim3_errmsg = "recursive decode failed"; goto finish; }
        if (verify) {
            uint8_t actual[SHA256_BLOCK_SIZE];
            sha256_buf(out->data, out->len, actual);
            if (memcmp(actual, sha, SHA256_BLOCK_SIZE) != 0) {
                aim3_errmsg = "SHA-256 mismatch"; ret = -1; goto finish;
            }
            s_dec_stats.sha_ok = 1;
        }
        goto finish;
    }

    ret = decode_body(container + pos, fl,
                      container + pos + fl, al,
                      orig, tb, fm, be,
                      verify, sha, out);
    if (ret == 0 && verify) s_dec_stats.sha_ok = 1;

finish:
    s_dec_stats.decoded_bytes = out->len;
    s_dec_stats.total_time_s  = clock_now() - t_total;
    return ret;
}

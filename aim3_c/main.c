#include "aim3.h"
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "portable_time.h"
#include <zlib.h>

/* ── file I/O helpers ───────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *len_out = got;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : -1;
}

static double clock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── log writers ────────────────────────────────────────────────────────── */

static void write_encode_log(const char *log_path, const char *in_path,
                              const char *out_path, const AIM3EncodeStats *st)
{
    FILE *f = fopen(log_path, "w");
    if (!f) { perror(log_path); return; }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(f, "# AIM3 run log -- %s\n", tsbuf);

    fprintf(f, "[run]\n");
    fprintf(f, "command      = encode\n");
    fprintf(f, "aim3_version = 6.7.5\n\n");

    fprintf(f, "[input]\n");
    fprintf(f, "path         = %s\n", in_path);
    fprintf(f, "bytes        = %zu\n\n", st->input_bytes);

    fprintf(f, "[bit_selection]\n");
    fprintf(f, "selected_bit = %d\n", st->selected_bit);
    fprintf(f, "time_s       = %.3f\n\n", st->bit_select_time_s);

    static const char *flag_name_map[] = {"gap+gz","bitset+gz","Elias-Fano","Gamma-RLE"};
    fprintf(f, "[flags]\n");
    fprintf(f, "mode         = %s\n",
            st->flag_mode >= 0 && st->flag_mode < 4 ? flag_name_map[st->flag_mode] : "?");
    fprintf(f, "bytes        = %zu\n", st->flag_bytes);
    fprintf(f, "time_s       = %.3f\n\n", st->flag_time_s);

    static const char *be_key[] = {"gzip","ans0","ans1",NULL,"ans2d","ans_stride"};
    fprintf(f, "[backends]\n");
    for (int i = 0; i < 6; i++) {
        if (i == 3 || st->backend_size[i] == 0) continue;
        fprintf(f, "%-12s = %zu\n", be_key[i], st->backend_size[i]);
    }
    fprintf(f, "winner       = %s\n",
            st->best_backend >= 0 && st->best_backend < 6 && be_key[st->best_backend]
            ? be_key[st->best_backend] : "?");
    if (st->best_backend == BACKEND_ANS_STRIDE)
        fprintf(f, "stride_k     = %u\n", (unsigned)st->stride_k);
    fprintf(f, "time_s       = %.3f\n\n", st->backend_time_s);

    fprintf(f, "[recursive]\n");
    fprintf(f, "used         = %s\n", st->used_recursive ? "yes" : "no");
    if (st->used_recursive) {
        fprintf(f, "n_layers     = %d\n", st->n_layers);
        fprintf(f, "payload_bytes = %zu\n", st->recursive_bytes);
        fprintf(f, "winner       = yes\n");
    }
    fprintf(f, "time_s       = %.3f\n\n", st->recursive_time_s);

    double ratio = st->input_bytes ? 100.0 * st->output_bytes / st->input_bytes : 0.0;
    fprintf(f, "[output]\n");
    fprintf(f, "path              = %s\n", out_path);
    fprintf(f, "container_version = 0x%02X\n", (unsigned)st->container_version);
    fprintf(f, "bytes             = %zu\n", st->output_bytes);
    fprintf(f, "ratio_pct         = %.2f\n", ratio);
    fprintf(f, "total_time_s      = %.3f\n", st->total_time_s);

    fclose(f);
    printf("  Log        : %s\n", log_path);
}

static void write_decode_log(const char *log_path, const char *in_path,
                              const char *out_path, const AIM3DecodeStats *st)
{
    FILE *f = fopen(log_path, "w");
    if (!f) { perror(log_path); return; }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(f, "# AIM3 run log -- %s\n", tsbuf);

    fprintf(f, "[run]\n");
    fprintf(f, "command      = decode\n");
    fprintf(f, "aim3_version = 6.7.5\n\n");

    fprintf(f, "[input]\n");
    fprintf(f, "path         = %s\n", in_path);
    fprintf(f, "bytes        = %zu\n\n", st->container_bytes);

    static const char *flag_name_map[] = {"gap+gz","bitset+gz","Elias-Fano","Gamma-RLE"};
    static const char *be_name_map[]   = {"gzip","ans0","ans1","?","ans2d","ans_stride"};
    fprintf(f, "[container]\n");
    fprintf(f, "version      = 0x%02X\n", (unsigned)st->container_version);
    if (st->n_layers > 0)
        fprintf(f, "n_layers     = %d\n", st->n_layers);
    fprintf(f, "flag_mode    = %s\n",
            st->flag_mode >= 0 && st->flag_mode < 4 ? flag_name_map[st->flag_mode] : "?");
    fprintf(f, "backend      = %s\n",
            st->backend >= 0 && st->backend < 6 ? be_name_map[st->backend] : "?");
    fprintf(f, "orig_bytes   = %zu\n\n", st->orig_bytes);

    fprintf(f, "[output]\n");
    fprintf(f, "path         = %s\n", out_path);
    fprintf(f, "bytes        = %zu\n", st->decoded_bytes);
    fprintf(f, "sha256_ok    = %s\n", st->sha_ok ? "yes" : "no");
    fprintf(f, "total_time_s = %.3f\n", st->total_time_s);

    fclose(f);
    printf("  Log        : %s\n", log_path);
}

/* ── usage ──────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
"AIM3 v6.7.5 — C port\n"
"Usage:\n"
"  %s encode  <input> <output.aim3> [options]\n"
"       --target-bit N      (0-7, default: auto)\n"
"       --backend NAME      (auto|gzip|ans0|ans1|ans2d|ans-stride, default: auto)\n"
"       --gz-level N        (1-9, default: 9)\n"
"       --verbose\n"
"       --sample-cap N      (bytes for bit selection, 0=full)\n"
"       --log <path>        (write structured run log to file)\n"
"\n"
"  %s decode  <input.aim3> <output> [options]\n"
"       --no-verify         (skip SHA-256 check)\n"
"       --log <path>        (write structured run log to file)\n"
"\n"
"  %s bench   <input> [--fast]\n"
"  %s flagbench <input>\n",
prog, prog, prog, prog);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    /* ── encode ── */
    if (strcmp(cmd, "encode") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *in_path  = argv[2];
        const char *out_path = argv[3];

        int         target_bit = -1;
        int         backend    = -1;
        int         gz_level   = AIM3_GZ_LEVEL;
        int         verbose    = 0;
        size_t      sample_cap = 0;
        const char *log_path   = NULL;

        for (int i = 4; i < argc; i++) {
            if      (strcmp(argv[i],"--target-bit")==0 && i+1<argc)
                { target_bit = atoi(argv[++i]); }
            else if (strcmp(argv[i],"--backend")==0 && i+1<argc) {
                const char *b = argv[++i];
                if      (strcmp(b,"gzip")      ==0) backend = 0;
                else if (strcmp(b,"ans0")      ==0) backend = 1;
                else if (strcmp(b,"ans1")      ==0) backend = 2;
                else if (strcmp(b,"ans2d")     ==0) backend = 4;
                else if (strcmp(b,"ans-stride")==0) backend = 5;
                else if (strcmp(b,"auto")      ==0) backend =-1;
                else { fprintf(stderr,"Unknown backend '%s'\n",b); return 1; }
            }
            else if (strcmp(argv[i],"--gz-level")==0 && i+1<argc)
                { gz_level = atoi(argv[++i]); }
            else if (strcmp(argv[i],"--verbose")==0)
                { verbose = 1; }
            else if (strcmp(argv[i],"--sample-cap")==0 && i+1<argc)
                { sample_cap = (size_t)atol(argv[++i]); }
            else if (strcmp(argv[i],"--log")==0 && i+1<argc)
                { log_path = argv[++i]; }
            else { fprintf(stderr,"Unknown option '%s'\n",argv[i]); return 1; }
        }

        struct stat st;
        if (stat(in_path, &st) < 0) { perror(in_path); return 1; }
        size_t n = (size_t)st.st_size;
        printf("Encoding '%s' (%zu bytes)\n", in_path, n);

        Buf out; buf_init(&out);
        int r = aim3_encode(NULL, n, target_bit, backend, gz_level, verbose,
                            sample_cap, in_path, &out);

        if (r < 0) {
            fprintf(stderr, "encode failed: %s\n", aim3_errmsg);
            buf_free(&out); return 1;
        }

        if (write_file(out_path, out.data, out.len) < 0) {
            fprintf(stderr,"write failed: %s\n", out_path);
            buf_free(&out); return 1;
        }
        printf("  Written '%s'\n", out_path);
        buf_free(&out);

        if (log_path)
            write_encode_log(log_path, in_path, out_path, aim3_encode_stats());
        return 0;
    }

    /* ── decode ── */
    if (strcmp(cmd, "decode") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *in_path  = argv[2];
        const char *out_path = argv[3];
        int         verify   = 1;
        const char *log_path = NULL;
        for (int i = 4; i < argc; i++) {
            if      (strcmp(argv[i],"--no-verify")==0) verify = 0;
            else if (strcmp(argv[i],"--log")==0 && i+1<argc) { log_path = argv[++i]; }
            else { fprintf(stderr,"Unknown option '%s'\n",argv[i]); return 1; }
        }

        size_t clen; uint8_t *container = read_file(in_path, &clen);
        if (!container) return 1;
        printf("Decoding '%s' (%zu bytes)\n", in_path, clen);

        Buf out; buf_init(&out);
        double t0 = clock_now();
        int r = aim3_decode(container, clen, verify, &out);
        double elapsed = clock_now() - t0;
        free(container);

        if (r < 0) {
            fprintf(stderr,"decode failed: %s\n", aim3_errmsg);
            buf_free(&out); return 1;
        }

        if (write_file(out_path, out.data, out.len) < 0) {
            fprintf(stderr,"write failed: %s\n", out_path);
            buf_free(&out); return 1;
        }

        const AIM3DecodeStats *ds = aim3_decode_stats();
        printf("  SHA-256    : %s\n",
               !verify ? "skipped" : (ds->sha_ok ? "verified" : "FAILED"));
        printf("  Recovered  : %zu bytes  [%.2fs]\n", out.len, elapsed);
        buf_free(&out);

        if (log_path)
            write_decode_log(log_path, in_path, out_path, ds);
        return 0;
    }

    /* ── bench ── */
    if (strcmp(cmd, "bench") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        const char *in_path = argv[2];
        int fast = 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i],"--fast")==0) fast = 1;
        size_t n; uint8_t *data = read_file(in_path, &n);
        if (!data) return 1;
        benchmark(data, n, fast);
        free(data);
        return 0;
    }

    /* ── flagbench ── */
    if (strcmp(cmd, "flagbench") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        size_t n; uint8_t *data = read_file(argv[2], &n);
        if (!data) return 1;
        flag_bench(data, n);
        free(data);
        return 0;
    }

    usage(argv[0]);
    return 1;
}

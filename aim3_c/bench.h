#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>

/*
 * flag_bench: for each of 8 bit-planes, print gz/EF/RLE encoded sizes
 *             and the winner — matches Python flag_bench() output format.
 */
void flag_bench(const uint8_t *data, size_t n);

/*
 * benchmark: run full encode with each backend, print summary table.
 * fast=1 skips ans1 and ans2d.
 */
void benchmark(const uint8_t *data, size_t n, int fast);

#endif /* BENCH_H */

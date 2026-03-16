# AIM — Adaptive Isolating Model

**A parameter-free framework for recursive bit-plane decomposition of arbitrary byte data.**

```
$ ./aim encode sample-full.wav sample.aim4

Encoded  'sample-full.wav'  (40,581,228 bytes)
  Mode   : recursive
  Output : 30,071,615 bytes  (74.10%)
  Delta  : -10,509,873 bytes  (-25.90% vs gzip)
  Time   : 8.68s
```

---

## What it is

Byte-level sliding window compressors (gzip, LZ77, DEFLATE) cannot access two classes of structure that are present in most non-random data:

- **Bit-plane density asymmetry** — the high byte and low byte of a PCM sample have completely different entropy profiles. Any compressor that treats them as the same stream is leaving structure on the table.
- **Inter-symbol value correlation at non-unit stride** — consecutive samples are numerically close; consecutive YUV pixels repeat with period 4; consecutive struct fields align at their width. These correlations are dissolved by the remap step if not captured first.

AIM exploits both. At each recursive depth it:

1. Sweeps all 8 bit positions to find the sparsest bit plane
2. Separates that plane as a flag set
3. **Before remapping**, computes an rANS order-1 stride-k encoding of the aligned stream — the last point where inter-symbol value correlations are intact
4. Compresses the flag set using the best of 7 available codecs
5. Remaps the aligned stream to a reduced symbol alphabet and recurses

During bottom-up assembly, the encoder finds the **optimal cutoff**: the earliest depth where the rANS encoding is smaller than the entire remaining subtree. If that depth exists, it halts there and discards the deeper levels. If it never exists, the output is identical to v14.

The stride *k* is selected by measuring H(X_i | X_{i−k}) across eight candidate values — no data type is named or detected. The same algorithm finds k=2 for PCM, k=4 for YUV420/RGBA, k=8 for 8-byte structs, k=1 for random data.

The algorithm is **parameter-free**: every decision — which bit, which codec, which stride, when to halt — is determined entirely by the data. The recursion terminates within 8 depths by proof.

---

## Build

```bash
gcc -O3 -o aim aim.c -lz -lm
```

Single file. Only dependency: zlib.

---

## Usage

```bash
# Encode
./aim encode input.bin output.aim

# Encode large file with bounded memory
./aim encode input.bin output.aim --disk

# Decode (SHA-256 verified)
./aim3 decode output.aim recovered.bin

# Benchmark both modes
./aim3 bench input.bin
```

---

## Results

| File                                      | Raw size | Output   | vs gzip                                     | Time |
| ----------------------------------------- | -------- | -------- | ------------------------------------------- | ---- |
| sample-full.wav (PCM audio)               | 38.7 MB  | 28.7 MB  | **−25.90%**                                 | 8.7s |
| fight_club_plane.yuv (uncompressed video) | 527 MB   | 194.7 MB | **−63.07%**                                 | 288s |
| Synthetic YUV420 (5 MB)                   | 5.0 MB   | 2.73 MB  | **−41.27%**                                 | —    |
| Synthetic PCM (5 MB)                      | 5.0 MB   | 3.63 MB  | **−22.70%**                                 | —    |
| Source code                               | 48 KB    | —        | +19% (expected — LZ77 wins on text)         | —    |
| Uniform byte                              | 10 KB    | —        | −98.5%                                      | —    |
| Random data                               | 100 KB   | —        | +0.14% (near-parity; ANS does not activate) | —    |

AIM is not a universal improvement over gzip. It is a structural improvement for data with bit-plane regularity or inter-symbol periodicity: PCM audio, YUV video, scientific measurements, image data, packed binary formats. For natural language and source code, gzip's LZ77 model wins and AIM correctly identifies this.

---

## How it works

**The bit-plane layer.** PCM audio stores 16-bit samples as interleaved byte pairs. The high byte clusters near zero during silence; the low byte is near-uniform regardless of signal level. These are structurally unlike streams. AIM's sweep finds and separates the sparsest bit plane across the full stream, regardless of byte boundaries. The flag codec race then exploits the positional regularity of where those bits are set. Recursion works on a stream with systematically reduced entropy at each depth.

**The stride layer.** Before each remap, AIM checks whether the aligned stream has inter-symbol periodicity. It measures H(X_i | X_{i−k}) for k ∈ {1, 2, 3, 4, 6, 8, 12, 16} on a 1 MB prefix, selects the k with the lowest conditional entropy, and encodes the full stream with rANS order-1 stride-k. During bottom-up assembly, if this encoding is cheaper than the remaining recursive structure, it becomes the output at that depth.

**Why pre-remap matters.** The remap operation halves the symbol alphabet by deleting bit b* from every value — this is what makes termination provable. But it also scrambles value correlations. By depth 2–3, the original PCM sample-pair or YUV pixel-stride correlation is effectively destroyed. Intercepting the aligned stream before remap is the correct intervention point. After remap it is too late.

---

## Flag codec race

At each depth, 7 codecs compete for the flag set. The shortest wins:

| Format                                    | Best for                             |
| ----------------------------------------- | ------------------------------------ |
| GAP — first position + varbyte gaps       | Sparse sets (< 5% density)           |
| BITSET — dense bit vector                 | General; always applicable           |
| ELIAS-FANO — monotone sequence encoding   | Clustered gap structure              |
| RLE — run-length on bitset                | Alternating run patterns             |
| HUFFMAN — canonical Huffman on flag bytes | Mid-density with byte repetition     |
| LZ77 — sliding window match-copy          | Positional repetition in flag stream |
| LZ77+HUFFMAN — DEFLATE (gzip)             | Large flag sets, mixed structure     |

---

## Formal properties

**Termination:** At depth *d*, the aligned stream has symbols in `[0, 2^(8−d) − 1]`. After remapping, the range is `[0, 2^(8−d−1) − 1]`. At depth 7, the symbol range is `[0, 1]`. HALT_DEPTH triggers at depth 8. The algorithm terminates within 8 depths for any input. HALT_ANS_STRIDE fires at or before the natural terminal and does not affect this proof.

**Losslessness:** Each level stores the selected bit, compressed flag set, and encoded child. For HALT_ANS_STRIDE levels, the ANS payload encodes the aligned stream directly (pre-remap); reconstruction skips unremap for that level only. `decode(encode(D)) = D` for all D.

**Parameter freedom:** No thresholds. No training data. No tuning constants. Bit selection, codec selection, stride selection, and halt decisions are all deterministic functions of the data and the algorithm.

**Optimal cutoff:** The bottom-up cutoff comparison uses the exact total cost of the remaining subtree at each depth — not an approximation. No earlier or later cutoff can produce a smaller output than the one selected.

---

## Halt conditions

| Code | Name            | Trigger                                         |
| ---- | --------------- | ----------------------------------------------- |
| 0    | HALT_RECURSE    | Default — not a terminal                        |
| 1    | HALT_TERMINAL   | MAX_DEPTH = 8 reached                           |
| 2    | HALT_ZERO       | Aligned stream is all-zero bytes                |
| 3    | HALT_ONE        | Aligned stream is all-one bytes                 |
| 4    | HALT_ANS_STRIDE | rANS stride-k encoding < remaining subtree cost |

---

## Implementations

| File | Description |
| --- | --- |
| `aim_v33.c` | C reference. Single file. `gcc -O3 -o aim aim_v33.c -lz -lm`. Zero warnings at `-Wall -Wextra`. Disk mode with bounded memory. |
| `aim_v15.py` | Python reference (older). Standard library + NumPy + zlib. Wire-compatible with v16. |
Both are wire-compatible. Files encoded by either implementation decode correctly with the other. All outputs are SHA-256 verified.

Python is the readable reference. C is ~29× faster on large files (8.7s vs 252s on the 38.7 MB WAV).

---

## Specification

`AIM_Specification` contains the complete wire format. Sufficient to implement a correct AIM decoder in any language without reference to the source files. Covers the rANS codec to implementable precision, the stride selection algorithm, all five halt conditions, and the ANS-stride payload wire format.

Container header (45 bytes): `AIM4` magic + mode (1B) + original length (8B BE) + SHA-256 (32B).

---

## Paper

`AIM_Framework_Paper` — *Adaptive Isolating Model (AIM): A Universal Algorithm for Bit-Plane Structure* — covers the theoretical foundations, formal proofs, the remap dissolution problem and why HALT_ANS_STRIDE addresses it, the non-domain justification for stride selection, relationship to existing work (bit-plane coding, DPCM, wavelets, LZ77, ANS), the Huffman analogy, and the full empirical evaluation including the fight_club YUV benchmark.

---

## The Huffman analogy

Huffman coding formalised the optimal parameter-free algorithm for symbol-frequency structure. AIM claims the analogous position for bit-plane and inter-symbol stride structure: universal over arbitrary byte data, lossless, parameter-free, deterministically terminating. The two methods are orthogonal — Huffman appears inside AIM as one of the seven flag codecs, and rANS appears as the HALT_ANS_STRIDE backend.

> *The goal is not to beat gzip. The goal is to know what you are compressing.*

---

## Hardware Considerations

The core AIM operations — bit-plane sweep, bit-clear, remap, and the rANS encode/decode loop — are branchless, fixed-depth, and operate on independent byte elements, making them amenable to SIMD vectorisation. Existing AVX-512 primitives (VGATHER, VPERMB, VPSHUFB) cover the stride-gather and remap patterns directly. A SIMD-accelerated C implementation and formal throughput analysis are left for future work.

---
## Stability and Compatibility

> **This is a research toolkit in active development. It is not an archival format or a production codec.**

### What this means in practice

**Wire format stability:** The AIM4 wire format is documented in `AIM_Specification` and has been stable since v16. Files encoded with v16 decode correctly with v16-compatible decoders. However, format revisions may occur as the algorithm develops. **Do not use AIM as an archival format for data you cannot recover from the original source.**

**Implementation stability:** The C implementation (`aim_v33.c` as of this writing) is iterative research code. Memory behaviour, performance characteristics, and edge-case handling are actively improving. Breaking changes may occur between versions without deprecation notice.

**Memory safety:** The `--disk` mode encoder has been extensively profiled and corrected through v25–v33 to bound peak RAM usage to approximately 2× the input file size. However, residual edge cases may exist. Monitor system memory during encoding of large files and ensure adequate free RAM before starting.

**Correctness verification:** Every encode/decode roundtrip is SHA-256 verified by the decoder. If the decoder reports a hash mismatch, the output is corrupt and should be discarded. Never trust an AIM-encoded file without running the decoder to verify it.

**Large file behaviour:** Files over 4 GB require v25 or later for correct decoding. Earlier versions contain an integer overflow in the decode path that silently truncates data beyond 4 GB.

### What this is good for right now

- Compression research and experimentation
- Evaluating bit-plane and inter-symbol structure in your data
- Academic and exploratory use where the source file is always retained
- Understanding the structural properties of binary formats (PCM, YUV, GGUF, etc.)

### What to use instead for archival or production use

If you need guaranteed long-term format stability, use a mature standard: `gzip`, `zstd`, `bzip2`, `xz`. AIM may eventually outperform them on specific data classes, but that is a research question, not a settled result.

---

---

## Intent

This work is published with the intent that it benefit general computing, research, and the people who do it.

The author explicitly does not endorse its use in mass surveillance systems, autonomous weapons, or any application designed primarily to cause harm to people. This is not a legal restriction — it is a statement of intent, on the record, in public.

---

## License

Code: [MIT License](LICENSE)

Paper and Specification: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

© 2026 Jacob Ritchey

---

## Citation

```
Ritchey, J. (2026). Adaptive Isolating Model (AIM): A Universal Algorithm
for Bit-Plane Structure. [Preprint]
```

# AIM — Adaptive Isolating Model 

**A parameter-free framework for recursive bit-plane decomposition of arbitrary byte data.**

```
$ ./aim encode sample-full.wav sample.aim4

Encoded  'sample-full.wav'  (40581228 bytes)
  Mode   : recursive
  Output : 34482879 bytes  (84.97%)
  Delta  : -6098609 bytes  (-15.03% vs gzip)
  Time   : 7.26s
```

---

## What it is

Byte-level sliding window compressors (gzip, LZ77, DEFLATE) cannot access structure that lives in the correlation between bit positions across a stream. That structure is real, present in most non-random data, and exploitable. AIM exploits it.

At each recursive depth, AIM:

1. Sweeps all 8 bit positions to find the sparsest bit plane in the input
2. Separates that plane as a flag set
3. Compresses the flag set using the best of 7 available codecs
4. Remaps the aligned stream to a reduced symbol alphabet and recurses

The algorithm is **parameter-free**: every decision — which bit to separate, which codec to use, when to halt — is determined entirely by the data. The recursion terminates within 8 depths for byte data by proof (the symbol space halves at each remap; no tunable depth limit is required).

The result is a complete, self-sufficient reconstruction program. No external dictionary, pre-trained model, or shared context. When it is shorter than the original, that is a structural finding about the data, not a compression artifact.

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

# Decode (SHA-256 verified)
./aim decode output.aim4 recovered.bin

# Benchmark both modes
./aim bench input.bin
```

---

## Results

| File | Size | vs gzip |
| --- | --- | --- |
| sample-full.wav (PCM audio) | 38.7 MB | **−15.03%** |
| Synthetic PCM (10 MB) | 10 MB | −13.56% |
| Source code | 48 KB | +19% (expected — LZ77 wins on text) |
| Uniform byte | 10 KB | −98.5% |

AIM is not a universal improvement over gzip. It is a structural improvement for data with bit-plane regularity — PCM audio, scientific measurements, image data, databases. For natural language and source code, gzip's LZ77 model wins and AIM correctly identifies this.

---

## How it works

The core insight: PCM audio stores 16-bit samples as interleaved byte pairs. The high byte (sample magnitude) clusters near zero during silence; the low byte (fractional) is near-uniform regardless of signal level. These are structurally unlike streams. Any compressor that treats them identically is conflating two different kinds of entropy.

AIM's bit-plane sweep finds and separates the sparsest plane across the full stream, regardless of byte boundaries. The positions of set bits in the high byte of audio data correspond to acoustic event onsets — that positional regularity is what the flag codec race exploits. The recursion then works on a stream with systematically reduced entropy at each depth.

The mechanism is the same for any data type with bit-plane structure. The algorithm discovers what structure is present rather than assuming it.

---

## Flag codec race

At each depth, 7 codecs compete for the flag set. The shortest wins:

| Format | Best for |
| --- | --- |
| GAP — first position + varbyte gaps | Sparse sets (< 5% density) |
| BITSET — dense bit vector | General; always applicable |
| ELIAS-FANO — monotone sequence encoding | Clustered gap structure |
| RLE — run-length on bitset | Alternating run patterns |
| HUFFMAN — canonical Huffman on flag bytes | Mid-density with byte repetition |
| LZ77 — sliding window match-copy | Positional repetition in flag stream |
| LZ77+HUFFMAN — DEFLATE (gzip) | Large flag sets, mixed structure |

---

## Formal properties

**Termination:** At depth *d*, the aligned stream has symbols in `[0, 2^(8−d) − 1]`. After remapping, the range is `[0, 2^(8−d−1) − 1]`. At depth 7, the symbol range is `[0, 1]` — a single-bit alphabet. HALT_DEPTH triggers at depth 8. The algorithm terminates within 8 depths for any input. This is a proof, not a parameter.

**Losslessness:** Each level stores the selected bit, compressed flag set, and encoded child. Reconstruction inverts bit_clear exactly at every depth. `decode(encode(D)) = D` for all D.

**Parameter freedom:** No thresholds. No training data. No tuning constants. Every decision is a deterministic function of the data and the algorithm.

---

## Implementations

| File | Description |
| --- | --- |
| `aim.c` | C reference. Single file, 1355 lines. `gcc -O3 -o aim aim.c -lz -lm` |
| `aim.py` | Python reference. Standard library + NumPy + zlib. |

Both are wire-compatible. Files encoded by either implementation decode correctly with the other. All outputs are SHA-256 verified.

Python is the readable reference. C is ~15× faster on large files.

---

## Specification

`AIM_Specification` contains the complete wire format. It is sufficient to implement a correct AIM decoder in any language or instruction set without reference to the source files.

Container header (45 bytes): `AIM4` magic + mode (1B) + original length (8B BE) + SHA-256 (32B).

---

## Paper

`AIM_Framework_Paper` — *Adaptive Isolating Model  (AIM): A Parameter-Free Framework for Recursive Bit-Plane Decomposition* — covers the theoretical foundations, formal proofs, relationship to existing work (bit-plane coding, DPCM, wavelets, LZ77), the Huffman analogy, and the full empirical evaluation.

---

## The Huffman analogy

Huffman coding formalised the optimal parameter-free algorithm for symbol-frequency structure. AIM claims the analogous position for bit-plane structure: universal over arbitrary byte data, lossless, parameter-free, deterministically terminating. The two methods are orthogonal — Huffman appears inside AIM as one of the seven flag codecs.

> *The goal is not to beat gzip. The goal is to know what you are compressing.*

---

## Intent

This work is published with the intent that it benefit general computing, research, and the people who do it.

The author explicitly does not endorse its use in mass surveillance systems, autonomous weapons, or any application designed primarily to cause harm to people. This is not a legal restriction — it is a statement of intent, on the record, in public.

---

## License

Code (`aim.c`, `aim.py`): [MIT License](LICENSE)

Paper and specification: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

© 2026 Jacob Ritchey

---

## Citation

```
Ritchey, J. (2026). Adaptive Isolating Model  (AIM): A Parameter-Free Framework
for Recursive Bit-Plane Decomposition. [Preprint]
```
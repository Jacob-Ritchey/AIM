"""
AIM v14  —  Adaptive Isolating Model, clean architecture
=========================================================
No backwards compatibility.  No legacy paths.  No ANS.  No block map.
Fresh implementation of the converged algorithm.

Architecture
------------
Two full-encode paths compete; the smaller output wins:

  MODE_RECURSIVE  per-level format competition with recursive descent
  MODE_CAIM       all flag bitsets concatenated, single gzip window

Both paths share the same O(n) sweep-and-clear core.

RECURSIVE path
  Sweep  O(n)  find the bit with fewest set positions
  Clear  O(n)  zero that bit; record flag positions as bitset
  Race   O(n)  four format workers run in parallel via scatter-gather,
               each writing into a pre-allocated result slot
  Remap  O(n)  pack out the cleared bit; halve the symbol space
  Halt?        MAX_DEPTH reached, or trivial terminal (all-zero / all-one)
  Recurse      if not halting, descend on the remapped aligned stream
  Terminal     gzip(aligned) at halt

  Pipelining: the sweep+remap for level k+1 runs on the main thread
  while the format workers fill their slots for level k.  Layer
  boundaries are pipeline stages, not synchronisation barriers.

CAIM path (Concatenated AIM)
  Same sweep loop but without remap (all 8 bits cleared independently
  from the original symbol space).  Flag bitsets are accumulated across
  levels and compressed in one gzip pass, exposing cross-level
  correlations invisible to per-level compression.

Flag formats (4 compete at each RECURSIVE level):
  0  gap+gz      gzip of delta-encoded gap list     (arithmetic runs)
  1  bitset+gz   gzip of raw n/8-byte bitset        (spatial runs)
  2  EF          Elias-Fano                         (sparse sets)
  3  RLE         Gamma-coded run lengths            (alternating runs)

Halt conditions (RECURSIVE):
  HALT_TERMINAL  gzip(remapped_aligned) stored as leaf
  HALT_ZERO      aligned stream is all zeros — perfect decomposition
  HALT_ONE       aligned stream is all ones  — 1-bit floor reached

Container wire format
---------------------
  [magic:    4 B]   b"AIM4"
  [mode:     1 B]   0=recursive  1=caim
  [orig_n:   8 B]   uint64 BE — original byte count
  [sha256:  32 B]   SHA-256 of original data
  [payload:  …  ]   depends on mode

Recursive payload (self-describing, depth-first):
  Per level:
    [bit:      1 B]   which bit was cleared (0-7)
    [fmt:      1 B]   flag format used (0-3)
    [flag_len: 4 B]   uint32 BE
    [flag_data:N B]
    [halt:     1 B]   0=recurse  1=terminal  2=zero  3=one
    [child_len:4 B]   uint32 BE  (0 when halt=zero or halt=one)
    [child:    N B]   recursive payload  OR  gzip(aligned)

CAIM payload:
  [n_levels: 1 B]
  [bits:     n_levels B]   which bit cleared at each level
  [flags_len:4 B]   uint32 BE
  [flags_gz: N B]   gzip(concat of all flag bitsets)
  [term_len: 4 B]   uint32 BE
  [term_gz:  N B]   gzip(final aligned stream)
"""

from __future__ import annotations

import argparse
import hashlib
import gzip
import heapq
import math
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Optional, Tuple

try:
    import numpy as np
    _HAS_NUMPY = True
except ImportError:
    _HAS_NUMPY = False

# Persistent thread pool — avoids per-level spawn overhead.
# gzip releases the GIL, so bitset+gz and gap+gz genuinely parallelise.
_POOL = None

def _get_pool() -> 'ThreadPoolExecutor':
    global _POOL
    if _POOL is None:
        _POOL = ThreadPoolExecutor(max_workers=4)
    return _POOL


# ─────────────────────────────────────────────────────────────────────────────
# CONSTANTS
# ─────────────────────────────────────────────────────────────────────────────

MAGIC        = b"AIM4"
HEADER_SIZE  = 45           # magic(4) + mode(1) + n(8) + sha256(32)

MODE_RECURSIVE = 0
MODE_CAIM      = 1

FMT_GAP      = 0            # VLC gap list (raw)
FMT_BITSET   = 1            # raw packed bitset bytes
FMT_EF       = 2            # Elias-Fano
FMT_RLE      = 3            # Gamma-coded run lengths
FMT_HUFFMAN  = 4            # order-0 canonical Huffman on bitset bytes
FMT_LZ77     = 5            # LZ77 on bitset bytes
FMT_LZ77HUFF = 6            # LZ77 tokens then Huffman entropy coded
FMT_NAMES    = {0:"gap", 1:"bitset", 2:"EF", 3:"RLE",
                4:"huffman", 5:"lz77", 6:"lz77+huff"}

HALT_RECURSE  = 0
HALT_TERMINAL = 1           # leaf: huffman(aligned)
HALT_ZERO     = 2           # leaf: aligned is all zeros
HALT_ONE      = 3           # leaf: aligned is all ones

MAX_DEPTH      = 8          # one level per bit in a byte symbol
BITSET_THRESH  = 0.40       # density above which gap list is not built
PARALLEL_MIN   = 512 * 1024 # bytes threshold before spawning threads
EF_MAX_DENSITY  = 0.15      # skip EF above this — materialising positions is too slow
GAP_MAX_DENSITY = 0.30      # skip GAP above this — gap list beats nothing at high density


# ─────────────────────────────────────────────────────────────────────────────
# UTILITIES
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# HUFFMAN (order-0 canonical)
# ─────────────────────────────────────────────────────────────────────────────

def _huff_build_lengths(data: bytes) -> bytes:
    """Return 256-byte code-length table for canonical Huffman."""
    freq = [0] * 256
    for b in data:
        freq[b] += 1
    heap = [[f, [i]] for i, f in enumerate(freq) if f > 0]
    if not heap:
        return bytes(256)
    heapq.heapify(heap)
    while len(heap) > 1:
        lo = heapq.heappop(heap)
        hi = heapq.heappop(heap)
        heapq.heappush(heap, [lo[0] + hi[0], lo[1] + hi[1]])
    lengths = [0] * 256
    def _assign(syms, depth):
        if len(syms) == 1:
            lengths[syms[0]] = max(1, min(depth, 15))
            return
        mid = len(syms) // 2
        _assign(syms[:mid], depth + 1)
        _assign(syms[mid:],  depth + 1)
    _assign(heap[0][1], 0)
    return bytes(lengths)


def _huff_codes(lengths: bytes) -> dict:
    """Canonical Huffman codes: sym -> (code_int, bit_length)."""
    codes = {}
    syms_by_len = sorted((l, i) for i, l in enumerate(lengths) if l > 0)
    code = 0; prev_len = 0
    for l, sym in syms_by_len:
        code <<= (l - prev_len)
        codes[sym] = (code, l)
        code += 1; prev_len = l
    return codes


def _huffman_encode(data: bytes) -> bytes:
    """Canonical Huffman encode.
    Wire: lengths[256] + n_bits(4B big-endian) + packed_bits."""
    if not data:
        return bytes(256) + struct.pack('>I', 0)
    lengths = _huff_build_lengths(data)
    codes   = _huff_codes(lengths)
    bits = []
    for b in data:
        c, cl = codes[b]
        for shift in range(cl - 1, -1, -1):
            bits.append((c >> shift) & 1)
    n_bits  = len(bits)
    packed  = bytearray(math.ceil(n_bits / 8))
    for i, b in enumerate(bits):
        if b:
            packed[i >> 3] |= 0x80 >> (i & 7)
    return lengths + struct.pack('>I', n_bits) + bytes(packed)


def _huffman_decode(payload: bytes) -> bytes:
    """Decode a canonical Huffman payload produced by _huffman_encode."""
    lengths = payload[:256]
    n_bits, = struct.unpack_from('>I', payload, 256)
    packed  = payload[260:]
    if n_bits == 0:
        return b''
    codes  = _huff_codes(lengths)
    dec    = {v: k for k, v in codes.items()}
    out    = bytearray()
    code   = 0; length = 0; bits_read = 0
    for byte_val in packed:
        for shift in range(7, -1, -1):
            if bits_read >= n_bits:
                break
            code = (code << 1) | ((byte_val >> shift) & 1)
            length += 1; bits_read += 1
            key = (code, length)
            if key in dec:
                out.append(dec[key])
                code = 0; length = 0
        if bits_read >= n_bits:
            break
    return bytes(out)


# ─────────────────────────────────────────────────────────────────────────────
# LZ77
# ─────────────────────────────────────────────────────────────────────────────

_LZ77_WIN   = 32768
_LZ77_MIN   = 3
_LZ77_MAX   = 258
_LZ77_CHAIN = 16   # hash-chain search depth


def _lz77_compress(data: bytes) -> bytes:
    """Hash-chain LZ77.  Wire: orig_n(4B) + flag-grouped token stream.
    Each group: 1 flag byte (MSB=first token), then per token:
      match  -> dist(2B BE) + (length - _LZ77_MIN)(1B)
      literal -> byte(1B)
    """
    n = len(data)
    tokens = []
    head   = {}   # 3-byte hash -> most recent position
    chain  = {}   # pos -> previous same-hash pos

    i = 0
    while i < n:
        if i + _LZ77_MIN > n:
            tokens.append((False, data[i], 0)); i += 1; continue
        h = (data[i] << 16) | (data[i+1] << 8) | data[i+2]
        best_len = 0; best_dist = 0
        j = head.get(h, -1); steps = 0
        while j != -1 and (i - j) <= _LZ77_WIN and steps < _LZ77_CHAIN:
            ml = 0
            while i + ml < n and data[j + ml] == data[i + ml] and ml < _LZ77_MAX:
                ml += 1
            if ml > best_len:
                best_len = ml; best_dist = i - j
            j = chain.get(j, -1); steps += 1
        chain[i] = head.get(h, -1); head[h] = i
        if best_len >= _LZ77_MIN:
            tokens.append((True, best_dist, best_len)); i += best_len
        else:
            tokens.append((False, data[i], 0)); i += 1

    out = bytearray(struct.pack('>II', n, len(tokens)))
    i = 0
    while i < len(tokens):
        chunk = tokens[i:i+8]
        flag  = 0
        for j, (is_match, _, _) in enumerate(chunk):
            if is_match:
                flag |= 1 << (7 - j)
        out.append(flag)
        for is_match, val, length in chunk:
            if is_match:
                out += struct.pack('>H', val)
                out.append(length - _LZ77_MIN)
            else:
                out.append(val)
        i += 8
    return bytes(out)


def _lz77_decompress(payload: bytes) -> bytes:
    """Reconstruct from _lz77_compress output."""
    orig_n, n_tokens = struct.unpack_from('>II', payload, 0)
    pos = 8; out = bytearray(); tok = 0
    while tok < n_tokens:
        if pos >= len(payload): break
        flag = payload[pos]; pos += 1
        for bit_i in range(7, -1, -1):
            if tok >= n_tokens: break
            if (flag >> bit_i) & 1:
                dist   = struct.unpack_from('>H', payload, pos)[0]; pos += 2
                length = payload[pos] + _LZ77_MIN;               pos += 1
                start  = len(out) - dist
                for k in range(length):
                    out.append(out[start + k])
            else:
                out.append(payload[pos]); pos += 1
            tok += 1
    return bytes(out)




# ─────────────────────────────────────────────────────────────────────────────
# BITSET SENTINEL
# Returned by _bit_clear when flag density >= BITSET_THRESH.
# Carries a pre-built bitset to avoid materialising a large position list.
# ─────────────────────────────────────────────────────────────────────────────

class _BitsetSentinel:
    __slots__ = ('bitset_bytes', 'n', '_pos')

    def __init__(self, hits: 'np.ndarray', n: int):
        # packbits with bitorder='little' matches manual buf[p>>3]|=(1<<(p&7))
        self.bitset_bytes = np.packbits(
            hits.view(np.uint8), bitorder='little').tobytes()
        self.n   = n
        self._pos = None

    def as_list(self) -> list:
        """Materialise position list (lazy, cached)."""
        if self._pos is None:
            bits = np.unpackbits(
                np.frombuffer(self.bitset_bytes, dtype=np.uint8),
                bitorder='little')[:self.n]
            self._pos = np.flatnonzero(bits).tolist()
        return self._pos

    def count(self) -> int:
        if _HAS_NUMPY:
            return int(np.unpackbits(
                np.frombuffer(self.bitset_bytes, dtype=np.uint8),
                bitorder='little')[:self.n].sum())
        return sum(bin(b).count('1') for b in self.bitset_bytes)


# ─────────────────────────────────────────────────────────────────────────────
# BIT OPERATIONS
# ─────────────────────────────────────────────────────────────────────────────

def _bit_clear(data: bytes, bit: int):
    """Zero every instance of `bit` in data.
    Returns (aligned_bytes, flag_positions).
    flag_positions is a numpy array, Python list, or _BitsetSentinel.
    """
    if _HAS_NUMPY and len(data) > 4096:
        arr  = np.frombuffer(data, dtype=np.uint8)
        mask = np.uint8(1 << bit)
        inv  = np.uint8((~(1 << bit)) & 0xFF)
        hits = (arr & mask) != 0
        out  = np.where(hits, arr & inv, arr).astype(np.uint8)
        if hits.mean() >= BITSET_THRESH:
            return bytes(out), _BitsetSentinel(hits, len(data))
        return bytes(out), np.flatnonzero(hits)
    mask = 1 << bit; inv = (~mask) & 0xFF
    aligned = bytearray(len(data)); flags = []
    for i, v in enumerate(data):
        if v & mask:
            aligned[i] = v & inv; flags.append(i)
        else:
            aligned[i] = v
    return bytes(aligned), flags


def _sweep(data: bytes, max_bit: int = 7) -> int:
    """Return the bit in [0, max_bit] with the fewest set positions."""
    if _HAS_NUMPY and len(data) > 4096:
        arr = np.frombuffer(data, dtype=np.uint8)
        counts = [int(np.count_nonzero(arr & np.uint8(1 << b)))
                  for b in range(max_bit + 1)]
    else:
        counts = [sum(1 for v in data if (v >> b) & 1)
                  for b in range(max_bit + 1)]
    return int(min(range(max_bit + 1), key=lambda b: counts[b]))


def _remap(aligned: bytes, bit: int) -> bytes:
    """Pack out the cleared bit slot; halves the symbol space.
    (high bits above bit) ++ (low bits below bit)
    """
    if _HAS_NUMPY and len(aligned) > 4096:
        arr  = np.frombuffer(aligned, dtype=np.uint8).astype(np.uint32)
        low  = arr & ((1 << bit) - 1)
        high = (arr >> (bit + 1)) << bit
        return (high | low).astype(np.uint8).tobytes()
    lo = (1 << bit) - 1
    out = bytearray(len(aligned))
    for i, v in enumerate(aligned):
        out[i] = ((v >> (bit + 1)) << bit) | (v & lo)
    return bytes(out)


def _unremap(remapped: bytes, bit: int) -> bytes:
    """Inverse of _remap: reinsert the cleared bit slot (value stays 0)."""
    if _HAS_NUMPY and len(remapped) > 4096:
        arr  = np.frombuffer(remapped, dtype=np.uint8).astype(np.uint32)
        low  = arr & ((1 << bit) - 1)
        high = (arr >> bit) << (bit + 1)
        return (high | low).astype(np.uint8).tobytes()
    lo = (1 << bit) - 1
    out = bytearray(len(remapped))
    for i, v in enumerate(remapped):
        out[i] = ((v >> bit) << (bit + 1)) | (v & lo)
    return bytes(out)


def _reconstruct(aligned: bytes, flag_pos, bit: int) -> bytes:
    """Restore the cleared bit at all flagged positions."""
    mask   = 1 << bit
    result = bytearray(aligned)
    if isinstance(flag_pos, _BitsetSentinel):
        flag_pos = flag_pos.as_list()
    if _HAS_NUMPY and hasattr(flag_pos, '__len__') and len(flag_pos) > 0:
        fp = np.asarray(flag_pos, dtype=np.int64)
        arr = np.frombuffer(result, dtype=np.uint8).copy()
        arr[fp] |= np.uint8(mask)
        return bytes(arr)
    for p in flag_pos:
        result[p] |= mask
    return bytes(result)


def _is_all_zero(data: bytes) -> bool:
    if _HAS_NUMPY:
        return not np.any(np.frombuffer(data, dtype=np.uint8))
    return all(b == 0 for b in data)


def _is_all_one(data: bytes) -> bool:
    if _HAS_NUMPY:
        return bool(np.all(np.frombuffer(data, dtype=np.uint8) == 1))
    return all(b == 1 for b in data)


# ─────────────────────────────────────────────────────────────────────────────
# ELIAS-FANO
# ─────────────────────────────────────────────────────────────────────────────

def _ef_encode(positions: list, N: int) -> bytes:
    k = len(positions)
    if k == 0:
        return struct.pack(">QQB", N, 0, 0)
    l = min(max(0, int(math.log2(N / k))), 30)
    lower_bytes = bytearray(math.ceil(k * l / 8) if l > 0 else 0)
    if l > 0:
        for i, pos in enumerate(positions):
            low_val = pos & ((1 << l) - 1)
            bo = i * l; val = low_val; bits_left = l
            while bits_left > 0:
                bi = bo >> 3; bb = bo & 7; chunk = min(bits_left, 8 - bb)
                lower_bytes[bi] |= (val & ((1 << chunk) - 1)) << bb
                val >>= chunk; bo += chunk; bits_left -= chunk
    high_parts  = [pos >> l for pos in positions]
    upper_size  = k + (N >> l) + 1
    upper_bytes = bytearray(math.ceil(upper_size / 8))
    bit_pos = 0; hi_idx = 0; bucket = 0; max_bucket = (N >> l) + 1
    while hi_idx < k or bucket <= max_bucket:
        while hi_idx < k and high_parts[hi_idx] == bucket:
            upper_bytes[bit_pos >> 3] |= (1 << (bit_pos & 7))
            bit_pos += 1; hi_idx += 1
        if hi_idx >= k:
            break
        bit_pos += 1; bucket += 1
    upper_bytes = upper_bytes[:math.ceil(bit_pos / 8)]
    return struct.pack(">QQB", N, k, l) + bytes(lower_bytes) + bytes(upper_bytes)


def _ef_decode(data: bytes) -> list:
    N, k, l = struct.unpack_from(">QQB", data, 0); pos = 17
    if k == 0:
        return []
    lbc = math.ceil(k * l / 8) if l > 0 else 0
    lower_bytes = data[pos:pos + lbc]; pos += lbc
    upper_bytes = data[pos:]
    lower_vals = []
    if l > 0:
        for i in range(k):
            val = 0; bits_left = l; bo = i * l; shift = 0
            while bits_left > 0:
                bi = bo >> 3; bb = bo & 7; chunk = min(bits_left, 8 - bb)
                bv = lower_bytes[bi] if bi < len(lower_bytes) else 0
                val |= ((bv >> bb) & ((1 << chunk) - 1)) << shift
                shift += chunk; bo += chunk; bits_left -= chunk
            lower_vals.append(val)
    else:
        lower_vals = [0] * k
    positions = []; bucket = 0; found = 0; bit_pos = 0
    total_bits = len(upper_bytes) * 8
    while found < k and bit_pos < total_bits:
        bi = bit_pos >> 3; bb = bit_pos & 7
        if bi >= len(upper_bytes):
            break
        if (upper_bytes[bi] >> bb) & 1:
            positions.append((bucket << l) | lower_vals[found]); found += 1
        else:
            bucket += 1
        bit_pos += 1
    return positions


# ─────────────────────────────────────────────────────────────────────────────
# GAMMA-CODED RLE
# ─────────────────────────────────────────────────────────────────────────────

def _gamma_encode(runs: list) -> bytes:
    bits = []
    for n in runs:
        n = max(1, int(n))
        k = n.bit_length() - 1
        bits.extend([0] * k)
        for shift in range(k, -1, -1):
            bits.append((n >> shift) & 1)
    out = bytearray(math.ceil(len(bits) / 8))
    for i, b in enumerate(bits):
        if b:
            out[i >> 3] |= (0x80 >> (i & 7))
    return bytes(out)


def _gamma_decode(data: bytes, n_runs: int) -> list:
    runs = []; bit_pos = 0; total_bits = len(data) * 8
    for _ in range(n_runs):
        k = 0
        while bit_pos < total_bits:
            byte_i = bit_pos >> 3; bit_i = 7 - (bit_pos & 7)
            bit = (data[byte_i] >> bit_i) & 1; bit_pos += 1
            if bit == 0:
                k += 1
            else:
                val = 1
                for _ in range(k):
                    if bit_pos >= total_bits:
                        break
                    byte_i = bit_pos >> 3; bit_i = 7 - (bit_pos & 7)
                    val = (val << 1) | ((data[byte_i] >> bit_i) & 1)
                    bit_pos += 1
                runs.append(val); break
    return runs


def _rle_encode(flag_pos, n: int) -> bytes:
    """Gamma-RLE of the flag bitstream alternating runs."""
    fp = list(flag_pos) if not isinstance(flag_pos, list) else flag_pos
    if len(fp) == 0:
        return struct.pack(">BI", 0, 1) + _gamma_encode([n])
    runs = []; first_bit = 0 if fp[0] > 0 else 1
    if first_bit == 0:
        runs.append(fp[0]); i = 0
        while i < len(fp):
            run_1 = 1
            while (i + run_1 < len(fp) and
                   fp[i + run_1] == fp[i] + run_1):
                run_1 += 1
            runs.append(run_1); i += run_1
            if i < len(fp):
                runs.append(fp[i] - (fp[i - 1] + 1))
    else:
        i = 0
        while i < len(fp):
            run_1 = 1
            while (i + run_1 < len(fp) and
                   fp[i + run_1] == fp[i] + run_1):
                run_1 += 1
            runs.append(run_1); i += run_1
            if i < len(fp):
                gap = fp[i] - (fp[i - 1] + 1)
                if gap > 0:
                    runs.append(gap)
    trailing = n - fp[-1] - 1
    if trailing > 0:
        runs.append(trailing)
    runs = [max(1, r) for r in runs if r > 0]
    return struct.pack(">BI", first_bit, len(runs)) + _gamma_encode(runs)


def _rle_decode(data: bytes, n: int) -> list:
    first_bit, n_runs = struct.unpack_from(">BI", data, 0)
    runs      = _gamma_decode(data[5:], n_runs)
    positions = []; pos = 0; cur = first_bit
    for run in runs:
        if cur == 1:
            for i in range(run):
                if pos + i < n:
                    positions.append(pos + i)
        pos += run; cur ^= 1
    return positions


# ─────────────────────────────────────────────────────────────────────────────
# FLAG ENCODING — per-format workers and race
# ─────────────────────────────────────────────────────────────────────────────

def _to_pos_list(flag_pos) -> list:
    """Normalise any flag_pos type to a plain Python list of ints."""
    if isinstance(flag_pos, _BitsetSentinel):
        return flag_pos.as_list()
    if _HAS_NUMPY and hasattr(flag_pos, 'tolist'):
        return flag_pos.tolist()
    return list(flag_pos)


def _build_bitset_bytes(flag_pos, n: int) -> bytes:
    """Build n/8-byte bitset from flag positions."""
    if isinstance(flag_pos, _BitsetSentinel):
        return flag_pos.bitset_bytes
    buf = bytearray(math.ceil(n / 8))
    for p in flag_pos:
        buf[int(p) >> 3] |= (1 << (int(p) & 7))
    return bytes(buf)



def _term_encode(data: bytes) -> bytes:
    """Terminal codec: LZ77 for repeated-structure removal, then Huffman for entropy.
    Used for: recursive halt terminals, CAIM flag concatenation, CAIM final stream."""
    if not data:
        return _huffman_encode(b"")
    return gzip.compress(data, 9)

def _term_decode(payload: bytes) -> bytes:
    """Inverse of _term_encode."""
    return gzip.decompress(payload)

def _encode_fmt(flag_pos, n: int, fmt: int) -> bytes:
    """Encode flag positions in a specific format. Returns raw bytes."""
    # Bitset-based formats (all operate on the packed n/8-byte bitset)
    if fmt in (FMT_BITSET, FMT_HUFFMAN, FMT_LZ77, FMT_LZ77HUFF):
        bs = _build_bitset_bytes(flag_pos, n)
        if fmt == FMT_BITSET:   return bs
        if fmt == FMT_HUFFMAN:  return _huffman_encode(bs)
        if fmt == FMT_LZ77:     return _lz77_compress(bs)
        if fmt == FMT_LZ77HUFF: return gzip.compress(bs, 9)

    fp = _to_pos_list(flag_pos)

    if fmt == FMT_GAP:
        k = len(fp)
        if k == 0:
            return struct.pack(">I", 0)
        gaps = [fp[0]] + [fp[i] - fp[i - 1] for i in range(1, k)]
        mx   = max(gaps)
        if mx <= 255:
            return struct.pack(">I", k) + bytes(gaps)
        elif mx <= 65534:
            return struct.pack(">I", k | 0x80000000) + b"".join(
                struct.pack(">H", g) for g in gaps)
        else:
            return struct.pack(">I", k | 0x40000000) + b"".join(
                struct.pack(">I", g) for g in gaps)

    if fmt == FMT_EF:
        return _ef_encode(fp, n)

    if fmt == FMT_RLE:
        return _rle_encode(fp, n)

    raise ValueError(f"Unknown fmt {fmt}")


def _decode_fmt(data: bytes, n: int, fmt: int) -> list:
    """Decode a flag payload back to a sorted position list."""
    if fmt in (FMT_BITSET, FMT_HUFFMAN, FMT_LZ77, FMT_LZ77HUFF):
        if fmt == FMT_BITSET:   bs = data
        elif fmt == FMT_HUFFMAN:  bs = _huffman_decode(data)
        elif fmt == FMT_LZ77:     bs = _lz77_decompress(data)
        else:                     bs = gzip.decompress(data)
        return [i for i in range(n) if (bs[i >> 3] >> (i & 7)) & 1]

    if fmt == FMT_GAP:
        raw   = data
        raw_k = struct.unpack_from(">I", raw, 0)[0]
        four_b = bool(raw_k & 0x40000000)
        two_b  = bool(raw_k & 0x80000000) and not four_b
        k = raw_k & 0x3FFFFFFF
        if k == 0:
            return []
        pos = 4; gaps = []
        if four_b:
            for _ in range(k):
                gaps.append(struct.unpack_from(">I", raw, pos)[0]); pos += 4
        elif two_b:
            for _ in range(k):
                gaps.append(struct.unpack_from(">H", raw, pos)[0]); pos += 2
        else:
            gaps = list(raw[pos:pos + k])
        cur = 0; out = []
        for g in gaps:
            cur += g; out.append(cur)
        return out

    if fmt == FMT_EF:
        return _ef_decode(data)

    if fmt == FMT_RLE:
        return _rle_decode(data, n)

    raise ValueError(f"Unknown fmt {fmt}")


def _select_fmts(density: float) -> list:
    """Select format candidates for the flag-set compression race.

    FMT_HUFFMAN and FMT_LZ77HUFF always compete: Huffman is the correct
    entropy coder for the bitset bytes; LZ77+Huffman additionally finds
    repeated subsequences in the bit-plane (spatial row-correlation in
    image-like data). FMT_RLE competes at all densities. FMT_GAP and
    FMT_EF only at low density where a position list is compact.
    FMT_LZ77 (without entropy stage) competes at mid-density only.
    FMT_BITSET (raw, no coding) is always included as a fallback.
    """
    fmts = [FMT_HUFFMAN, FMT_LZ77HUFF, FMT_RLE, FMT_BITSET]
    if density <= GAP_MAX_DENSITY:
        fmts += [FMT_GAP, FMT_EF]
    if 0.15 <= density <= 0.85:
        fmts.append(FMT_LZ77)
    return fmts


def _flag_race(flag_pos, n: int, density: float, use_threads: bool = True) -> Tuple[bytes, int]:
    """Race format encoders; return (best_bytes, best_fmt).

    Formats are pruned by density before dispatching — avoids materialising
    large position lists for EF/GAP when density makes them structural losers.
    Uses the module-level persistent ThreadPoolExecutor.
    """
    fmts = _select_fmts(density)

    if use_threads and n >= PARALLEL_MIN and len(fmts) > 1:
        pool = _get_pool()
        future_to_fmt = {
            pool.submit(_encode_fmt, flag_pos, n, fmt): fmt
            for fmt in fmts
        }
        results = {future_to_fmt[f]: f.result() for f in as_completed(future_to_fmt)}
    else:
        results = {fmt: _encode_fmt(flag_pos, n, fmt) for fmt in fmts}

    best_fmt = min(results, key=lambda f: len(results[f]))
    return results[best_fmt], best_fmt


# ─────────────────────────────────────────────────────────────────────────────
# RECURSIVE ENCODE / DECODE
# ─────────────────────────────────────────────────────────────────────────────

def _recursive_encode(data: bytes) -> bytes:
    """Encode data iteratively; only O(n) working memory at any time.

    Wire format per level (outer-first in the byte stream):
        bit(1) fmt(1) flag_len(4) halt(1) child_len(4)  flag_data[flag_len]

    halt values:
        HALT_RECURSE  = 0   child is the next level header
        HALT_TERMINAL = 1   child is gzip(remapped stream), level has valid flags
        HALT_ZERO     = 2   input was all zeros, no child
        HALT_ONE      = 3   input was all ones,  no child
    """
    n = len(data)

    if n == 0:
        terminal = gzip.compress(b"", 9)
        return struct.pack(">BBIBI", 0, FMT_GAP, 0, HALT_TERMINAL, len(terminal)) + terminal

    # ── Iterative descent: collect one (bit, fmt, flag_block) per level ──
    levels: List[Tuple[int, int, bytes]] = []
    current  = data
    del data

    for depth in range(MAX_DEPTH):
        mb = 7 - depth   # effective max bit shrinks as symbol space halves

        if mb < 0:
            break
        if _is_all_zero(current) or _is_all_one(current):
            break

        best_bit = _sweep(current, mb)

        aligned, flag_pos = _bit_clear(current, best_bit)

        if isinstance(flag_pos, _BitsetSentinel):
            k = flag_pos.count()
        elif _HAS_NUMPY and hasattr(flag_pos, '__len__'):
            k = len(flag_pos)
        else:
            k = len(flag_pos)
        density = k / len(current) if current else 0.0

        remapped = _remap(aligned, best_bit)
        del aligned

        use_threads = (len(current) >= PARALLEL_MIN)
        if use_threads:
            pool = _get_pool()
            future_to_fmt = {
                pool.submit(_encode_fmt, flag_pos, len(current), fmt): fmt
                for fmt in _select_fmts(density)
            }
            fmt_results = {future_to_fmt[f]: f.result()
                           for f in as_completed(future_to_fmt)}
        else:
            fmt_results = {fmt: _encode_fmt(flag_pos, len(current), fmt)
                           for fmt in _select_fmts(density)}

        del flag_pos

        best_fmt   = min(fmt_results, key=lambda f: len(fmt_results[f]))
        flag_block = fmt_results[best_fmt]
        del fmt_results

        levels.append((best_bit, best_fmt, flag_block))

        current = remapped
        del remapped

    if _is_all_zero(current):
        inner = struct.pack(">BBIBI", 0, FMT_GAP, 0, HALT_ZERO, 0)
    elif _is_all_one(current):
        inner = struct.pack(">BBIBI", 0, FMT_GAP, 0, HALT_ONE, 0)
    else:
        gz_cur = _term_encode(current)
        inner  = struct.pack(">BBIBI", 0, FMT_GAP, 0, HALT_TERMINAL, len(gz_cur)) + gz_cur
    del current

    # ── Assemble bottom-up ────────────────────────────────────────────────
    payload = inner
    for best_bit, best_fmt, flag_block in reversed(levels):
        hdr     = struct.pack(">BBIBI", best_bit, best_fmt,
                              len(flag_block), HALT_RECURSE, len(payload))
        payload = hdr + flag_block + payload

    return payload

def _recursive_decode(payload: bytes, n: int) -> bytes:
    """Decode iteratively; O(n) peak memory.

    Always parses the full 11-byte level header; uses the `halt` byte at
    offset 6 — never the first byte — to distinguish levels from terminals.
    This avoids ambiguity when bit values (0-7) collide with halt codes.
    """
    if n == 0:
        return b""

    levels = []   # list of (bit, fmt, flag_data)
    pos    = 0

    while True:
        best_bit  = payload[pos]
        fm        = payload[pos + 1]
        flag_len, = struct.unpack_from(">I", payload, pos + 2)
        halt      = payload[pos + 6]
        child_len,= struct.unpack_from(">I", payload, pos + 7)
        flag_data  = bytes(payload[pos + 11: pos + 11 + flag_len])
        pos       += 11 + flag_len

        if halt == HALT_RECURSE:
            levels.append((best_bit, fm, flag_data))
            # pos now points to the child level header; continue walking

        else:
            # Terminal: HALT_ZERO, HALT_ONE, or HALT_TERMINAL.
            # flag_len==0 means this is a "pure" terminal with no
            # flags to apply; flag_len>0 means this level also has
            # flags (from a cake halt where we stop mid-recursion).
            child = bytes(payload[pos: pos + child_len])

            if halt == HALT_ZERO:
                current = bytes(n)
            elif halt == HALT_ONE:
                current = bytes([1] * n)
            else:  # HALT_TERMINAL
                current = _term_decode(child)

            if flag_len > 0:
                levels.append((best_bit, fm, flag_data))
            break

    # Rebuild bottom-up: unremap then restore flags for each level
    for best_bit, fm, flag_data in reversed(levels):
        aligned  = _unremap(current, best_bit)
        flag_pos = _decode_fmt(flag_data, n, fm)
        current  = _reconstruct(aligned, flag_pos, best_bit)
        del aligned, flag_pos

    return current

def _caim_encode(data: bytes) -> bytes:
    """CAIM: concatenate all flag bitsets across levels, compress in one pass.

    Does NOT remap between levels; each level clears a different bit from
    the full [0,255] symbol space.  Bits already cleared are excluded from
    subsequent sweeps.

    Returns the CAIM payload (without container header).
    """
    n          = len(data)
    current    = data
    bitsets    = []
    which_bits = []
    cleared    = 0         # bitmask of already-cleared bit positions

    for depth in range(MAX_DEPTH):
        # Sweep only bits not yet cleared
        avail = [b for b in range(8) if not (cleared >> b) & 1]
        if not avail:
            break

        if _HAS_NUMPY:
            arr    = np.frombuffer(current, dtype=np.uint8)
            counts = {b: int(np.count_nonzero(arr & np.uint8(1 << b)))
                      for b in avail}
        else:
            counts = {b: sum(1 for v in current if (v >> b) & 1)
                      for b in avail}

        best_bit = min(counts, key=counts.__getitem__)
        if counts[best_bit] == 0 and depth == 0:
            # This bit is already always-zero in the input; trivial
            pass

        aligned, flag_pos = _bit_clear(current, best_bit)
        bitsets.append(_build_bitset_bytes(flag_pos, n))
        which_bits.append(best_bit)
        cleared |= (1 << best_bit)
        current  = aligned

        if _is_all_zero(current) or _is_all_one(current):
            break

    n_levels  = len(which_bits)
    bits_hdr  = bytes(which_bits)
    flags_cat = b"".join(bitsets)
    flags_gz  = _term_encode(flags_cat)
    term_gz   = _term_encode(current)

    return (struct.pack(">B", n_levels) +
            bits_hdr +
            struct.pack(">I", len(flags_gz)) + flags_gz +
            struct.pack(">I", len(term_gz))  + term_gz)


def _caim_decode(payload: bytes, n: int) -> bytes:
    """Decode a CAIM payload."""
    n_levels = payload[0]; pos = 1
    which_bits = list(payload[pos: pos + n_levels]); pos += n_levels

    flags_len, = struct.unpack_from(">I", payload, pos); pos += 4
    flags_gz   = payload[pos: pos + flags_len]; pos += flags_len
    term_len,  = struct.unpack_from(">I", payload, pos); pos += 4
    term_gz    = payload[pos: pos + term_len]

    # Decompress and split flag bitsets
    bs_size    = math.ceil(n / 8)
    flags_cat  = _term_decode(flags_gz)
    bitsets    = [flags_cat[i * bs_size: (i + 1) * bs_size]
                  for i in range(n_levels)]

    # Decode terminal aligned stream
    current = _term_decode(term_gz)

    # Reconstruct bottom-up (reverse order)
    for i in range(n_levels - 1, -1, -1):
        bit  = which_bits[i]
        bs   = bitsets[i]
        fp   = [j for j in range(n) if (bs[j >> 3] >> (j & 7)) & 1]
        current = _reconstruct(current, fp, bit)

    return current


# ─────────────────────────────────────────────────────────────────────────────
# TOP-LEVEL ENCODE / DECODE
# ─────────────────────────────────────────────────────────────────────────────

def encode(data: bytes) -> bytes:
    """Encode data.  Both modes run; the smaller result is returned.

    Returns a self-contained AIM4 container.
    """
    n      = len(data)
    sha256 = hashlib.sha256(data).digest()

    payload_r = _recursive_encode(data)
    payload_c = _caim_encode(data)

    if len(payload_r) <= len(payload_c):
        mode    = MODE_RECURSIVE
        payload = payload_r
    else:
        mode    = MODE_CAIM
        payload = payload_c

    header = MAGIC + struct.pack(">BQ", mode, n) + sha256
    return header + payload


def decode(container: bytes, verify: bool = True) -> bytes:
    """Decode an AIM4 container."""
    if container[:4] != MAGIC:
        raise ValueError("Not an AIM4 file (bad magic).")

    mode = container[4]
    n,   = struct.unpack_from(">Q", container, 5)
    sha  = container[13: 45]
    payload = container[45:]

    if mode == MODE_RECURSIVE:
        result = _recursive_decode(payload, n)
    elif mode == MODE_CAIM:
        result = _caim_decode(payload, n)
    else:
        raise ValueError(f"Unknown mode {mode:#04x}")

    if verify:
        actual = hashlib.sha256(result).digest()
        if actual != sha:
            raise ValueError(
                f"SHA-256 mismatch.\n  Expected: {sha.hex()}\n  Got:      {actual.hex()}")
    return result


# ─────────────────────────────────────────────────────────────────────────────
# DIAGNOSTICS
# ─────────────────────────────────────────────────────────────────────────────

def _recursive_inspect(data: bytes, depth: int = 0, max_bit: int = 7,
                       rows: list = None) -> list:
    """Collect per-level statistics from the recursive decomposition."""
    if rows is None:
        rows = []
    n = len(data)
    if max_bit < 0 or n == 0 or _is_all_zero(data) or _is_all_one(data):
        return rows

    best_bit   = _sweep(data, max_bit)
    aligned, flag_pos = _bit_clear(data, best_bit)
    remapped   = _remap(aligned, best_bit)

    if isinstance(flag_pos, _BitsetSentinel):
        k = flag_pos.count()
    elif _HAS_NUMPY and hasattr(flag_pos, '__len__'):
        k = len(flag_pos)
    else:
        k = len(flag_pos)

    fmts = {fmt: _encode_fmt(flag_pos, n, fmt)
            for fmt in [FMT_GAP, FMT_BITSET, FMT_EF, FMT_RLE]}
    best_fmt = min(fmts, key=lambda f: len(fmts[f]))
    gz_size  = len(_huffman_encode(remapped))

    rows.append({
        'depth':      depth,
        'bit':        best_bit,
        'n':          n,
        'flags':      k,
        'density':    k / n if n else 0.0,
        'best_fmt':   FMT_NAMES[best_fmt],
        'flag_bytes': len(fmts[best_fmt]),
        'gz_aligned': gz_size,
    })

    if depth < max_bit:
        _recursive_inspect(remapped, depth + 1, max_bit - 1, rows)
    return rows


def _caim_inspect(data: bytes) -> list:
    """Collect per-level statistics from the CAIM decomposition."""
    n = len(data); rows = []; current = data; cleared = 0
    for depth in range(MAX_DEPTH):
        avail = [b for b in range(8) if not (cleared >> b) & 1]
        if not avail:
            break
        if _HAS_NUMPY:
            arr    = np.frombuffer(current, dtype=np.uint8)
            counts = {b: int(np.count_nonzero(arr & np.uint8(1 << b))) for b in avail}
        else:
            counts = {b: sum(1 for v in current if (v >> b) & 1) for b in avail}
        best_bit = min(counts, key=counts.__getitem__)
        aligned, flag_pos = _bit_clear(current, best_bit)
        bs  = _build_bitset_bytes(flag_pos, n)
        k   = counts[best_bit]
        cleared |= (1 << best_bit); current = aligned
        rows.append({
            'depth':   depth,
            'bit':     best_bit,
            'flags':   k,
            'density': k / n if n else 0.0,
            'bs_bytes': len(bs),
        })
        if _is_all_zero(current) or _is_all_one(current):
            break
    return rows


def bench(data: bytes) -> None:
    """Full benchmark: recursive, CAIM, and raw gzip."""
    n      = len(data)
    raw_gz = len(_huffman_encode(data))
    print(f"\nAIM v13 Benchmark  —  {n:,} bytes  ({n / 1024**2:.2f} MiB)")
    print(f"Raw gzip : {raw_gz:,} bytes  ({100 * raw_gz / n:.2f}%)\n")

    configs = [
        ("recursive",  lambda: _recursive_encode(data)),
        ("caim",       lambda: _caim_encode(data)),
    ]

    print(f"{'mode':<14}  {'payload':>12}  {'+ header':>10}  "
          f"{'total':>12}  {'ratio':>8}  {'vs gzip':>9}  {'time':>7}  {'ok':>3}")
    print("─" * 88)

    oh = HEADER_SIZE
    for label, fn in configs:
        t0 = time.monotonic()
        pl = fn()
        el = time.monotonic() - t0
        total  = len(pl) + oh
        delta  = total - raw_gz
        # Verify roundtrip
        container = MAGIC + struct.pack(">BQ", (MODE_RECURSIVE if label == "recursive"
                                                else MODE_CAIM), n) + hashlib.sha256(data).digest() + pl
        try:
            rec = decode(container)
            ok  = "✓" if rec == data else "✗"
        except Exception as e:
            ok  = f"✗ {e}"
        print(f"{label:<14}  {len(pl):>12,}  {oh:>10}  {total:>12,}  "
              f"{100 * total / n:>7.2f}%  {100 * delta / n:>+8.2f}%  "
              f"[{el:.1f}s]  {ok}")

    print()
    # Full encode (both compete)
    t0 = time.monotonic()
    c  = encode(data)
    el = time.monotonic() - t0
    mode_name = "recursive" if c[4] == MODE_RECURSIVE else "caim"
    total = len(c)
    print(f"{'auto (winner)':<14}  {'—':>12}  {'—':>10}  {total:>12,}  "
          f"{100 * total / n:>7.2f}%  {100 * (total - raw_gz) / n:>+8.2f}%  "
          f"[{el:.1f}s]  mode={mode_name}")
    print()


def inspect(data: bytes) -> None:
    """Per-level breakdown for both encode modes."""
    n = len(data)
    print(f"\nAIM v13 Inspect  —  {n:,} bytes  ({n / 1024**2:.2f} MiB)\n")

    # Recursive
    print("── Recursive decomposition ─────────────────────────────────────────────────")
    print(f"  {'depth':>6}  {'bit':>4}  {'n_syms':>10}  {'flags':>10}  "
          f"{'density':>8}  {'best_fmt':>12}  {'flag_B':>8}  {'gz_aln':>10}")
    print("  " + "─" * 78)
    for row in _recursive_inspect(data):
        print(f"  {row['depth']:>6}  {row['bit']:>4}  {row['n']:>10,}  "
              f"{row['flags']:>10,}  {100 * row['density']:>7.1f}%  "
              f"{row['best_fmt']:>12}  {row['flag_bytes']:>8,}  "
              f"{row['gz_aligned']:>10,}")

    print()
    # CAIM
    print("── CAIM decomposition ──────────────────────────────────────────────────────")
    print(f"  {'depth':>6}  {'bit':>4}  {'flags':>10}  {'density':>8}  {'bs_bytes':>10}")
    print("  " + "─" * 52)
    for row in _caim_inspect(data):
        print(f"  {row['depth']:>6}  {row['bit']:>4}  {row['flags']:>10,}  "
              f"{100 * row['density']:>7.1f}%  {row['bs_bytes']:>10,}")
    print()


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        prog="aim_v13",
        description="AIM v13 — clean recursive structural decomposition.",
        epilog="""
Examples:
  python aim_v13.py encode input.bin out.aim4
  python aim_v13.py decode out.aim4 recovered.bin
  python aim_v13.py bench input.bin
  python aim_v13.py inspect input.bin
""")
    sub = p.add_subparsers(dest="command", required=True)

    enc = sub.add_parser("encode", help="Encode a file")
    enc.add_argument("input")
    enc.add_argument("output")
    enc.add_argument("--verbose", action="store_true")

    dec = sub.add_parser("decode", help="Decode a file")
    dec.add_argument("input")
    dec.add_argument("output")
    dec.add_argument("--no-verify", action="store_true")

    bnc = sub.add_parser("bench", help="Benchmark both modes vs gzip")
    bnc.add_argument("input")

    ins = sub.add_parser("inspect", help="Per-level structural breakdown")
    ins.add_argument("input")

    args = p.parse_args()

    if args.command == "encode":
        t0   = time.monotonic()
        data = open(args.input, "rb").read()
        c    = encode(data)
        open(args.output, "wb").write(c)
        raw_gz = len(_huffman_encode(data))
        mode_name = "recursive" if c[4] == MODE_RECURSIVE else "caim"
        print(f"Encoded  '{args.input}'  ({len(data):,} bytes)")
        print(f"  Mode     : {mode_name}")
        print(f"  Output   : {len(c):,} bytes  ({100 * len(c) / len(data):.2f}%)")
        print(f"  Raw gzip : {raw_gz:,} bytes  ({100 * raw_gz / len(data):.2f}%)")
        print(f"  Delta    : {len(c) - raw_gz:>+,} bytes  "
              f"({100 * (len(c) - raw_gz) / len(data):>+.2f}%)")
        print(f"  Written  : '{args.output}'  [{time.monotonic() - t0:.2f}s]")

    elif args.command == "decode":
        t0 = time.monotonic()
        c  = open(args.input, "rb").read()
        data = decode(c, verify=not args.no_verify)
        open(args.output, "wb").write(data)
        v = "✓ verified" if not args.no_verify else "(unverified)"
        print(f"Decoded '{args.input}'  →  {len(data):,} bytes  {v}  "
              f"[{time.monotonic() - t0:.2f}s]")

    elif args.command == "bench":
        bench(open(args.input, "rb").read())

    elif args.command == "inspect":
        inspect(open(args.input, "rb").read())


if __name__ == "__main__":
    main()

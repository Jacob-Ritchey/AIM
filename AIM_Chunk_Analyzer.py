"""
AIM Chunking Extension
======================
Extends aim_core_v3.py with chunk-level structural analysis and adaptive encoding.

Motivation
----------
The global run_all() pipeline treats the entire input as a single unit.  For
large files — video in particular — this produces a blended decay profile that
masks structural variation between regions.  A 90-minute raw video contains
frames that range from static title cards (gradient-class, halt_depth ≤ 5) to
high-motion action sequences (noise-class, halt_depth > 10).  Analysed
globally, the profile reflects the mixture, not either component.  The optimal
primary_target_bit for a static frame may be bit 2 (quantization boundary);
for a high-motion frame it may be bit 0 or no bit at all.  A single global
choice is suboptimal for all but the statistically dominant frame type.

Chunking solves three distinct problems:

  1. Locality — per-chunk structural fingerprints reveal *where* exploitable
     structure lives inside a large file, not just whether any exists.  The
     resulting "complexity map" is a format-agnostic structural timeline.

  2. Adaptive bit selection — the primary_target_bit that minimises flag count
     varies by region.  Per-chunk selection is strictly equal-or-better than a
     single global bit choice.

  3. Parallelism — each chunk is structurally independent.  run_all() is
     synchronous.  Chunked analysis distributes across workers trivially.

For video, the natural chunk unit is the frame (width × height × channels
bytes per frame).  The module provides both frame-aligned and fixed-size
chunking, plus a "complexity series" — a structural timeline that maps byte
offset → {fingerprint class, halt_depth, optimal bit, entropy outcome}.

A fourth benefit specific to chunked encoding is the "AIM skip" decision:
chunks classified as "Uniform noise / encrypted" (halt_depth > 10, flat bit
distribution) receive no benefit from AIM decomposition and incur overhead.
The chunked encoder detects these chunks and falls back to raw gzip, avoiding
the flag storage cost entirely.  For heavily compressed or encrypted video
containers, this can reclaim 10–30 % of per-file overhead.

Design notes
------------
  • Lightweight analysis skips operation chaining, structure probe, compression
    benchmark, and invariant check — these add O(N) to O(N·max_depth) cost
    each.  The fast path runs in ~4× less time than run_all() at equivalent N.
  • All per-chunk results use the same dict structure as run_all() where keys
    overlap, for direct compatibility with existing analysis tooling.
  • Encoding format (AIMC v1) is self-describing: each chunk header records the
    bit target used, the original length, and the encoded length.  The decoder
    requires no external metadata.

Python required: 3.9+
Dependencies   : aim_core_v3  (same directory or on PYTHONPATH), stdlib only.
"""

from __future__ import annotations

import gzip
import math
import os
import struct
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from typing import Dict, List, Optional, Tuple

try:
    from tqdm import tqdm as _tqdm
    _HAS_TQDM = True
except ImportError:
    _HAS_TQDM = False

from aim_core_v3 import (
    real_transform,
    bit_clear_sweep,
    bit_clear_pass,
    compute_all_bit_entropies,
    classify_fingerprint,
    _encode_as_bitset,
    _encode_as_delta   as _encode_as_delta_core,  # imported but overridden below
    _encode_flags_raw,       # bitset-vs-delta selector
    _encode_flags_targeted,  # optimal per-layer selector
    _gz,                     # gzip convenience wrapper
)


def _encode_as_delta(flags: List[int]) -> bytes:
    """
    Corrected delta encoder — handles deltas > 65535 (large chunks).

    aim_core_v3._encode_as_delta uses struct.pack('>H', d) for 2-byte mode,
    which overflows when the space is larger than 65535 bytes (e.g. 2.3 MB
    video frames).  This version adds a 4-byte mode for large deltas.

    Format:
      [4 bytes big-endian uint32] count
      [count × bytes_each]        delta values

    bytes_each = 1 if all deltas ≤ 255
               = 2 if all deltas ≤ 65535
               = 4 otherwise
    """
    if not flags:
        return b""
    deltas = [flags[0]] + [flags[i] - flags[i - 1] for i in range(1, len(flags))]
    max_delta = max(deltas)
    if max_delta <= 255:
        bytes_each = 1
        fmt = "B"
    elif max_delta <= 65535:
        bytes_each = 2
        fmt = ">H"
    else:
        bytes_each = 4
        fmt = ">I"
    header = struct.pack(">I", len(flags))
    if bytes_each == 1:
        body = bytes(deltas)
    else:
        body = b"".join(struct.pack(fmt, d) for d in deltas)
    return header + body


# ─────────────────────────────────────────────────────────────────────────────
# 1.  LIGHTWEIGHT ANALYSIS — fast per-chunk profiler
# ─────────────────────────────────────────────────────────────────────────────

def flag_position_profile(flags: List[int], n: int,
                           skip_rle: bool = False) -> Dict:
    """
    Analyse the spatial distribution of L0 flag positions for a single bit
    target.  This is the core measurement for format design — it answers
    *what kind of data are the flag positions* so we can pick the cheapest
    representation.

    Parameters
    ----------
    flags : sorted list of flag positions (0-based indices into the chunk)
    n     : total chunk length (the space the flags live in)

    Returns
    -------
    dict with keys:

    Density
      k                – number of flags
      n                – space size
      density          – k / n  (0 = all clear, 1 = all set)

    Gap analysis  (gaps between consecutive flag positions)
      gap_min          – smallest gap
      gap_max          – largest gap
      gap_mean         – mean gap (≈ n/k for uniform)
      gap_stddev       – standard deviation of gaps
      gap_cv           – coefficient of variation (stddev / mean)
                         ~0 = perfectly periodic, ~1 = exponential/random,
                         >1 = highly clustered/bursty
      gap_uniformity   – 1 - (gap_cv / (1 + gap_cv)), normalised 0→1
                         1 = perfectly uniform spacing, 0 = maximally bursty

    Run-length analysis  (consecutive runs of set / clear positions)
      run_set_max      – longest run of consecutive flagged positions
      run_set_mean     – mean run length of flagged positions
      run_clear_max    – longest run of consecutive unflagged positions
      run_clear_mean   – mean run length of unflagged positions

    Clustering
      spatial_entropy  – Shannon entropy of gap distribution (bits).
                         Low = gaps are predictable / periodic.
                         High = gaps are unpredictable / random.
      leading_gap_pct  – fraction of gaps that are 1 (immediately adjacent
                         flags). High = strong spatial clustering.

    Encoding size comparison  (bytes, no framing overhead)
      enc_bitset       – flat bitset:   ceil(n/8)
      enc_delta_raw    – delta-encoded: 4 + k * bytes_each
      enc_bitset_gz    – bitset + gzip9
      enc_delta_gz     – delta  + gzip9
      enc_best         – smallest of the four
      enc_best_method  – label for the winner
      enc_vs_raw_pct   – enc_best / enc_bitset * 100  (savings vs baseline)

    Per-layer encoding breakdown  (all layers, not just L0)
      layers           – list of dicts, one per recursive layer:
                           depth, k, n, density,
                           enc_best, enc_best_method
    """
    if not flags or n == 0:
        return {
            "k": 0, "n": n, "density": 0.0,
            "gap_min": 0, "gap_max": 0, "gap_mean": 0.0,
            "gap_stddev": 0.0, "gap_cv": 0.0, "gap_uniformity": 1.0,
            "run_set_max": 0, "run_set_mean": 0.0,
            "run_clear_max": 0, "run_clear_mean": 0.0,
            "spatial_entropy": 0.0, "leading_gap_pct": 0.0,
            "enc_bitset": math.ceil(n / 8), "enc_delta_raw": 4,
            "enc_bitset_gz": math.ceil(n / 8), "enc_delta_gz": 4,
            "enc_best": 0, "enc_best_method": "empty",
            "enc_vs_raw_pct": 0.0, "layers": [],
        }

    k = len(flags)

    # ── Gap analysis ──────────────────────────────────────────────────────────
    gaps = [flags[0]] + [flags[i] - flags[i-1] for i in range(1, k)]
    # include trailing gap to end of space
    gap_mean   = sum(gaps) / len(gaps) if gaps else 0.0
    gap_var    = sum((g - gap_mean) ** 2 for g in gaps) / len(gaps) if gaps else 0.0
    gap_stddev = math.sqrt(gap_var)
    gap_cv     = gap_stddev / gap_mean if gap_mean > 0 else 0.0
    gap_uniformity = 1.0 - (gap_cv / (1.0 + gap_cv))

    # gap entropy (treat gap values as a discrete distribution)
    from collections import Counter
    gap_counts = Counter(gaps)
    total_gaps = len(gaps)
    spatial_entropy = -sum(
        (c / total_gaps) * math.log2(c / total_gaps)
        for c in gap_counts.values() if c > 0
    )
    leading_gap_pct = gap_counts.get(1, 0) / total_gaps if total_gaps > 0 else 0.0

    # ── Run-length analysis ───────────────────────────────────────────────────
    if skip_rle:
        run_set_max = run_clear_max = 0
        run_set_mean = run_clear_mean = 0.0
    else:
        flag_set = set(flags)
        set_runs, clear_runs = [], []
        cur_run, cur_type = 1, (0 in flag_set)
        for i in range(1, n):
            t = (i in flag_set)
            if t == cur_type:
                cur_run += 1
            else:
                (set_runs if cur_type else clear_runs).append(cur_run)
                cur_run, cur_type = 1, t
        (set_runs if cur_type else clear_runs).append(cur_run)

        run_set_max   = max(set_runs)   if set_runs   else 0
        run_set_mean  = sum(set_runs)   / len(set_runs)   if set_runs   else 0.0
        run_clear_max = max(clear_runs) if clear_runs else 0
        run_clear_mean= sum(clear_runs) / len(clear_runs) if clear_runs else 0.0

    # ── Encoding sizes ────────────────────────────────────────────────────────
    bs_raw  = _encode_as_bitset(flags, n)
    dl_raw  = _encode_as_delta(flags)          # uses local corrected version
    bs_gz   = _gz(bs_raw) if len(bs_raw) > 32 else bs_raw
    dl_gz   = _gz(dl_raw) if len(dl_raw) > 32 else dl_raw

    candidates = {
        "bitset-raw":   len(bs_raw),
        "delta-raw":    len(dl_raw),
        "bitset+gzip":  len(bs_gz),
        "delta+gzip":   len(dl_gz),
    }
    enc_best_method = min(candidates, key=candidates.__getitem__)
    enc_best        = candidates[enc_best_method]
    enc_bitset      = len(bs_raw)
    enc_vs_raw_pct  = (enc_best / enc_bitset * 100) if enc_bitset > 0 else 100.0

    return {
        "k":               k,
        "n":               n,
        "density":         round(k / n, 6),
        "gap_min":         min(gaps),
        "gap_max":         max(gaps),
        "gap_mean":        round(gap_mean, 2),
        "gap_stddev":      round(gap_stddev, 2),
        "gap_cv":          round(gap_cv, 4),
        "gap_uniformity":  round(gap_uniformity, 4),
        "run_set_max":     run_set_max,
        "run_set_mean":    round(run_set_mean, 2),
        "run_clear_max":   run_clear_max,
        "run_clear_mean":  round(run_clear_mean, 2),
        "spatial_entropy": round(spatial_entropy, 4),
        "leading_gap_pct": round(leading_gap_pct, 4),
        "enc_bitset":      enc_bitset,
        "enc_delta_raw":   len(dl_raw),
        "enc_bitset_gz":   len(bs_gz),
        "enc_delta_gz":    len(dl_gz),
        "enc_best":        enc_best,
        "enc_best_method": enc_best_method,
        "enc_vs_raw_pct":  round(enc_vs_raw_pct, 2),
    }


def lightweight_analyze(data: List[int], skip_rle: bool = False) -> Dict:
    """
    Fast structural analysis for a single data chunk.

    Runs the four passes that define a chunk's structural identity:
      • REAL transform   → decay profile, halt depth, L0 odd ratio
      • Bit-clear sweep  → total flag counts for all 8 bit positions
      • All-bit entropy  → entropy outcome (net %) for all 8 bit positions
      • Fingerprint      → classify against reference library

    Skips (vs run_all):
      - Operation chaining (linear chain sweep, disconnected chain) —
        confirmed uniformly worse in experiments; irrelevant per-chunk.
      - Structure probe  — mod-N alignment measure; useful for whole-file
        analysis but adds cost with no per-chunk decision value.
      - Compression benchmark — too expensive for per-chunk use.
      - Byte-alignment invariant — meaningful only when chunk length is a
        multiple of 8, which is guaranteed for frame-aligned video chunks
        but not fixed-size ones; caller can run compute_invariant() if needed.

    Parameters
    ----------
    data : list[int]
        Byte values (0–255) for one chunk.

    Returns
    -------
    dict with keys:
      decay_layers      – from real_transform()
      bit_sweep         – from bit_clear_sweep(); list[8] with total flag counts
      all_bit_entropies – from compute_all_bit_entropies(); list[8]
      fingerprint       – from classify_fingerprint(); dict or None
      primary_target_bit – int: bit position with lowest total flag count
      best_entropy_bit  – int: bit position with lowest net_pct (most negative)
      best_entropy_pct  – float: net_pct of best_entropy_bit (negative = reduction)
      size              – int: len(data)
    """
    if not data:
        return {
            "decay_layers":       [],
            "bit_sweep":          [],
            "all_bit_entropies":  [],
            "fingerprint":        None,
            "primary_target_bit": 0,
            "best_entropy_bit":   0,
            "best_entropy_pct":   0.0,
            "size":               0,
        }

    decay_layers      = real_transform(data)
    bit_sweep         = bit_clear_sweep(data)
    all_bit_entropies = compute_all_bit_entropies(data)
    fingerprint       = classify_fingerprint(decay_layers)

    # Primary target bit: sweep winner (fewest total flags).
    primary_target_bit = min(bit_sweep, key=lambda r: (r["total"], r["bit"]))["bit"]

    # Best entropy bit: winner on Shannon entropy net % (most negative = most reduction).
    best_entropy_entry = min(all_bit_entropies, key=lambda r: r["net_pct"])
    best_entropy_bit   = best_entropy_entry["bit"]
    best_entropy_pct   = best_entropy_entry["net_pct"]

    # Flag position profiles for both winners.
    # We recompute L0 flags directly from bit_clear_pass to get actual positions.
    n = len(data)
    _, sweep_flags  = bit_clear_pass(data, primary_target_bit)
    _, entropy_flags = bit_clear_pass(data, best_entropy_bit)
    sweep_flag_profile   = flag_position_profile(sweep_flags,   n, skip_rle=skip_rle)
    entropy_flag_profile = flag_position_profile(entropy_flags, n, skip_rle=skip_rle)
    # Also profile per-layer for the entropy winner (deeper structure)
    layer_profiles = []
    current = entropy_flags
    depth = 1
    while current and depth < 40:
        _, next_flags = bit_clear_pass(current, 0)  # REAL (bit-0) on positions
        lp = flag_position_profile(current, len(data) if depth == 1 else layer_profiles[-1]["n"])
        lp["depth"] = depth
        layer_profiles.append(lp)
        if not next_flags:
            break
        current = next_flags
        depth += 1

    return {
        "decay_layers":        decay_layers,
        "bit_sweep":           bit_sweep,
        "all_bit_entropies":   all_bit_entropies,
        "fingerprint":         fingerprint,
        "primary_target_bit":  primary_target_bit,
        "best_entropy_bit":    best_entropy_bit,
        "best_entropy_pct":    round(best_entropy_pct, 4),
        "size":                n,
        "sweep_flag_profile":  sweep_flag_profile,
        "entropy_flag_profile": entropy_flag_profile,
        "layer_profiles":      layer_profiles,
    }


# ─────────────────────────────────────────────────────────────────────────────
# 2.  CHUNK SPLITTERS
# ─────────────────────────────────────────────────────────────────────────────

def fixed_size_chunks(data: List[int], chunk_size: int) -> List[Tuple[int, List[int]]]:
    """
    Split *data* into fixed-size chunks.

    The last chunk may be smaller than *chunk_size*.

    Returns
    -------
    list of (offset, chunk_data) tuples.
    """
    if chunk_size <= 0:
        raise ValueError(f"chunk_size must be > 0, got {chunk_size}")
    result = []
    for i in range(0, len(data), chunk_size):
        result.append((i, data[i : i + chunk_size]))
    return result


def video_frame_chunks(
    data: List[int],
    frame_size: int,
    *,
    width: Optional[int] = None,
    height: Optional[int] = None,
    channels: int = 3,
) -> List[Tuple[int, List[int]]]:
    """
    Split *data* into video-frame-aligned chunks.

    Two calling conventions:
      • Provide frame_size directly (bytes per frame).
      • Provide width, height, channels; frame_size = width × height × channels.
        channels defaults to 3 (RGB).  Provide channels=1 for greyscale,
        channels=4 for RGBA.

    Frame-aligned chunking is preferred over fixed-size chunking for raw video
    because:
      (a) Each chunk is semantically atomic — one frame — so the fingerprint
          corresponds to a single temporal unit.
      (b) For raw video, frame_size is always a multiple of 8 (even for
          greyscale at width divisible by 8), making the byte-alignment
          invariant applicable per chunk.
      (c) The resulting complexity series maps directly to frame indices,
          which can be correlated with codec metadata (I/P/B frame types).

    Parameters
    ----------
    data       : list[int]   raw video bytes (any planar or packed format)
    frame_size : int         bytes per frame; overridden by width/height if given
    width      : int|None    frame width in pixels (optional convenience)
    height     : int|None    frame height in pixels (optional convenience)
    channels   : int         bytes per pixel (default 3 = RGB)

    Returns
    -------
    list of (byte_offset, frame_data) tuples.
    """
    if width is not None and height is not None:
        frame_size = width * height * channels

    if frame_size <= 0:
        raise ValueError(f"frame_size must be > 0, got {frame_size}")

    chunks = []
    n_complete = len(data) // frame_size
    for i in range(n_complete):
        offset = i * frame_size
        chunks.append((offset, data[offset : offset + frame_size]))

    remainder_start = n_complete * frame_size
    if remainder_start < len(data):
        chunks.append((remainder_start, data[remainder_start:]))

    return chunks


def adaptive_chunk_size(data_length: int, target_chunks: int = 16,
                        min_size: int = 512, max_size: int = 1 << 20) -> int:
    """
    Suggest a fixed chunk size for files without natural frame boundaries.

    Targets *target_chunks* approximately equal-sized chunks, clamped to
    [min_size, max_size].  This heuristic is appropriate for non-video binary
    files where the right granularity is unknown in advance.

    For video, always prefer video_frame_chunks() with the actual frame size.

    Returns
    -------
    int: suggested chunk_size in bytes
    """
    raw = data_length // max(1, target_chunks)
    return max(min_size, min(max_size, raw))


# ─────────────────────────────────────────────────────────────────────────────
# 3.  BATCH CHUNK ANALYSIS
# ─────────────────────────────────────────────────────────────────────────────

def chunk_analyze(
    data: List[int],
    chunk_size: Optional[int] = None,
    *,
    frame_size: Optional[int] = None,
    width: Optional[int] = None,
    height: Optional[int] = None,
    channels: int = 3,
    target_chunks: int = 16,
    workers: Optional[int] = None,
    skip_rle: bool = False,
) -> List[Dict]:
    """
    Analyse a file chunk-by-chunk and return per-chunk structural profiles.

    Exactly one of the following must be specified:
      • chunk_size          — fixed-size chunking
      • frame_size          — video frame-aligned chunking
      • width + height      — video frame-aligned (frame_size computed)

    If none are specified, chunk_size is estimated via adaptive_chunk_size().

    Parameters
    ----------
    data          : list[int]   full file as byte values
    chunk_size    : int|None    bytes per chunk (fixed-size mode)
    frame_size    : int|None    bytes per frame (video mode)
    width, height : int|None    frame dimensions (video mode, alternative)
    channels      : int         bytes per pixel for video mode (default 3)
    target_chunks : int         target chunk count for adaptive sizing
    workers       : int|None    worker processes for parallel analysis
                                (default: os.cpu_count(); pass 1 to disable)
    skip_rle      : bool        skip run-length analysis in flag profiles
                                (faster for large files)

    Returns
    -------
    list of dicts, one per chunk, each containing:
      offset              – byte offset into original data
      index               – chunk index (0-based)
      size                – chunk size in bytes
      decay_layers        – REAL transform layers
      bit_sweep           – bit-clear sweep results (list[8])
      all_bit_entropies   – entropy outcomes (list[8])
      fingerprint         – structural classification dict
      primary_target_bit  – sweep-optimal bit (int)
      best_entropy_bit    – entropy-optimal bit (int)
      best_entropy_pct    – entropy net% for best_entropy_bit (float)
      sweep_flag_profile  – flag position profile for sweep winner
      entropy_flag_profile– flag position profile for entropy winner
      layer_profiles      – per-layer encoding profiles
    """
    if width is not None and height is not None:
        raw_chunks = video_frame_chunks(data, frame_size or 0,
                                        width=width, height=height,
                                        channels=channels)
    elif frame_size is not None:
        raw_chunks = video_frame_chunks(data, frame_size)
    else:
        cs = chunk_size or adaptive_chunk_size(len(data), target_chunks)
        raw_chunks = fixed_size_chunks(data, cs)

    return _analyze_chunks_parallel(raw_chunks, workers=workers,
                                    skip_rle=skip_rle)


def _analyze_one(args: Tuple[int, int, List[int], bool]) -> Dict:
    """Top-level worker function (must be picklable — no closures)."""
    idx, offset, chunk, skip_rle = args
    analysis = lightweight_analyze(chunk, skip_rle=skip_rle)
    return {"index": idx, "offset": offset, **analysis}


def _analyze_chunks_parallel(
    raw_chunks: List[Tuple[int, List[int]]],
    workers: Optional[int] = None,
    skip_rle: bool = False,
) -> List[Dict]:
    """
    Run lightweight_analyze on each chunk in parallel using worker processes.

    Uses ProcessPoolExecutor to bypass the GIL and saturate available CPU
    cores.  Falls back to single-process execution when there is only one
    chunk or one worker is explicitly requested (avoids fork overhead on
    small inputs).

    Progress is displayed via tqdm if available, otherwise a compact
    stdlib fallback printer is used (updates every 2 seconds).

    Parameters
    ----------
    raw_chunks : list of (offset, chunk_data) tuples
    workers    : number of worker processes (default: os.cpu_count())
    skip_rle   : skip run-length analysis in flag profiles

    Returns
    -------
    list of per-chunk result dicts, in original chunk order.
    """
    n_workers = workers or os.cpu_count() or 1
    n_chunks  = len(raw_chunks)

    # Single-process fast path
    if n_chunks <= 1 or n_workers == 1:
        results = []
        for idx, (offset, chunk) in enumerate(_progress_iter(
                enumerate(raw_chunks), total=n_chunks,
                desc="Analysing chunks")):
            results.append(_analyze_one((idx, offset, chunk, skip_rle)))
        return results

    work = [(idx, offset, chunk, skip_rle)
            for idx, (offset, chunk) in enumerate(raw_chunks)]

    actual_workers = min(n_workers, n_chunks)
    results: List[Optional[Dict]] = [None] * n_chunks

    with ProcessPoolExecutor(max_workers=actual_workers) as pool:
        future_to_idx = {pool.submit(_analyze_one, item): item[0]
                         for item in work}
        for future in _progress_iter(
                as_completed(future_to_idx), total=n_chunks,
                desc="Analysing chunks"):
            idx = future_to_idx[future]
            results[idx] = future.result()

    return results  # type: ignore[return-value]


def _progress_iter(iterable, *, total: int, desc: str = ""):
    """
    Wrap *iterable* with a progress display.

    Uses tqdm when available.  Falls back to a plain stderr printer that
    updates at most once every 2 seconds so it doesn't flood the terminal.
    """
    if _HAS_TQDM:
        yield from _tqdm(iterable, total=total, desc=desc,
                         unit="chunk", dynamic_ncols=True)
        return

    # Stdlib fallback — no external dependencies required
    import sys
    done      = 0
    start     = time.monotonic()
    last_print = start - 999  # force immediate first print

    for item in iterable:
        yield item
        done += 1
        now = time.monotonic()
        if now - last_print >= 2.0 or done == total:
            elapsed  = now - start
            rate     = done / elapsed if elapsed > 0 else 0
            eta_s    = (total - done) / rate if rate > 0 else 0
            eta_str  = f"{int(eta_s // 60)}m{int(eta_s % 60):02d}s"
            pct      = 100 * done / total if total else 0
            mb_done  = 0  # byte tracking not available here
            print(
                f"\r{desc}: {done}/{total}  ({pct:.1f}%)  "
                f"{rate:.1f} chunks/s  ETA {eta_str}   ",
                end="", file=sys.stderr, flush=True
            )
            last_print = now

    if total > 0:
        print(file=sys.stderr)  # newline after completion


# ─────────────────────────────────────────────────────────────────────────────
# 4.  COMPLEXITY MAP AND TRANSITION DETECTION
# ─────────────────────────────────────────────────────────────────────────────

def complexity_series(chunk_results: List[Dict]) -> List[Dict]:
    """
    Distil per-chunk analysis into a compact structural timeline.

    Returns one row per chunk with only the fields relevant to structure
    navigation: offset, size, fingerprint class, halt depth, optimal bit, and
    entropy outcome.  Suitable for plotting a "complexity heatmap" over time.

    Returns
    -------
    list of dicts:
      index               – chunk index
      offset              – byte offset
      size                – chunk size
      cls                 – fingerprint class label (str) or "unknown"
      halt_depth          – int; proxy for structural depth
      l0_odd_ratio        – float
      primary_target_bit  – sweep-optimal bit
      best_entropy_bit    – entropy-optimal bit
      best_entropy_pct    – entropy net % (negative → genuine reduction territory)
    """
    series = []
    for c in chunk_results:
        fp = c.get("fingerprint") or {}
        series.append({
            "index":              c["index"],
            "offset":             c["offset"],
            "size":               c["size"],
            "cls":                fp.get("cls", "unknown"),
            "halt_depth":         fp.get("halt_depth", -1),
            "l0_odd_ratio":       fp.get("l0_odd_ratio", 0.0),
            "primary_target_bit": c["primary_target_bit"],
            "best_entropy_bit":   c["best_entropy_bit"],
            "best_entropy_pct":   c["best_entropy_pct"],
        })
    return series


def detect_structural_transitions(
    chunk_results: List[Dict],
    *,
    halt_depth_delta: int = 3,
    bit_change: bool = True,
    class_change: bool = True,
) -> List[Dict]:
    """
    Identify chunk boundaries where structural class changes significantly.

    A transition is flagged when any of the following occurs between chunk i
    and chunk i+1 (thresholds are configurable):

      class_change     : fingerprint cls label changes (e.g. noise → structured)
      halt_depth_delta : |halt_depth[i+1] - halt_depth[i]| ≥ halt_depth_delta
      bit_change       : primary_target_bit changes

    These criteria jointly detect:
      • Scene cuts in video (abrupt class transition between frame types)
      • Compression boundary artefacts (structured header → noise payload)
      • Quantization level changes in physical measurement data

    Parameters
    ----------
    chunk_results    : output of chunk_analyze()
    halt_depth_delta : minimum halt_depth change to flag (default 3 layers)
    bit_change       : flag when primary_target_bit changes (default True)
    class_change     : flag when fingerprint cls label changes (default True)

    Returns
    -------
    list of transition dicts, each containing:
      between_chunks   – (index_before, index_after)
      byte_offset      – offset of the second chunk (transition point)
      reasons          – list of strings describing why this was flagged
      before           – complexity_series entry for chunk i
      after            – complexity_series entry for chunk i+1
    """
    series = complexity_series(chunk_results)
    transitions = []

    for i in range(len(series) - 1):
        a, b = series[i], series[i + 1]
        reasons = []

        if class_change and a["cls"] != b["cls"]:
            reasons.append(
                f"class change: '{a['cls']}' → '{b['cls']}'"
            )

        depth_diff = abs(b["halt_depth"] - a["halt_depth"])
        if depth_diff >= halt_depth_delta:
            reasons.append(
                f"halt_depth Δ={depth_diff} ({a['halt_depth']} → {b['halt_depth']})"
            )

        if bit_change and a["primary_target_bit"] != b["primary_target_bit"]:
            # Suppress within-class bit changes for noise/encrypted regions:
            # the bit distribution is flat there, so the primary_target_bit
            # winner is sampling noise rather than a meaningful structural signal.
            a_noise = "noise" in a["cls"].lower() or "encrypted" in a["cls"].lower()
            b_noise = "noise" in b["cls"].lower() or "encrypted" in b["cls"].lower()
            if not (a_noise and b_noise):
                reasons.append(
                    f"bit change: {a['primary_target_bit']} → {b['primary_target_bit']}"
                )

        if reasons:
            transitions.append({
                "between_chunks": (a["index"], b["index"]),
                "byte_offset":    b["offset"],
                "reasons":        reasons,
                "before":         a,
                "after":          b,
            })

    return transitions


def summarize_chunked_analysis(chunk_results: List[Dict]) -> Dict:
    """
    Aggregate statistics over all chunks.

    Returns
    -------
    dict:
      n_chunks             – total chunks analysed
      total_bytes          – sum of all chunk sizes
      class_distribution   – {cls_label: count}
      bit_distribution     – {bit_index: count} for primary_target_bit
      halt_depth_mean      – float
      halt_depth_min       – int
      halt_depth_max       – int
      n_reduction_chunks   – chunks where best_entropy_pct < -1.0 %
      n_noise_chunks       – chunks classified as "Uniform noise / encrypted"
      compression_skip_pct – n_noise_chunks / n_chunks × 100 (encoding skip rate)
    """
    series = complexity_series(chunk_results)
    if not series:
        return {}

    class_dist: Dict[str, int] = {}
    bit_dist:   Dict[int, int] = {}
    halt_depths = []
    n_reduction = 0
    n_noise     = 0

    for row in series:
        cls = row["cls"]
        class_dist[cls] = class_dist.get(cls, 0) + 1

        ptb = row["primary_target_bit"]
        bit_dist[ptb] = bit_dist.get(ptb, 0) + 1

        halt_depths.append(row["halt_depth"])

        if row["best_entropy_pct"] < -1.0:
            n_reduction += 1
        if "noise" in cls.lower() or "encrypted" in cls.lower():
            n_noise += 1

    n = len(series)
    return {
        "n_chunks":             n,
        "total_bytes":          sum(r["size"] for r in chunk_results),
        "class_distribution":   class_dist,
        "bit_distribution":     bit_dist,
        "halt_depth_mean":      round(sum(halt_depths) / n, 2),
        "halt_depth_min":       min(halt_depths),
        "halt_depth_max":       max(halt_depths),
        "n_reduction_chunks":   n_reduction,
        "n_noise_chunks":       n_noise,
        "compression_skip_pct": round(n_noise / n * 100, 2),
    }


# ─────────────────────────────────────────────────────────────────────────────
# 5.  ADAPTIVE BOUNDARY DETECTION (variable-size chunking)
# ─────────────────────────────────────────────────────────────────────────────

def adaptive_chunk_boundaries(
    data: List[int],
    initial_size: int,
    *,
    halt_depth_delta: int = 4,
    min_size: int = 256,
) -> List[int]:
    """
    Find chunk boundary positions using structural transition detection.

    Instead of fixed-size chunks, this function runs a two-pass approach:

      Pass 1 — Analyse fixed chunks of *initial_size* to get a coarse
               complexity series.
      Pass 2 — Wherever a structural transition is detected (by the same
               criteria as detect_structural_transitions()), record the
               corresponding byte offset as a natural boundary.

    Fixed chunk boundaries that fall between two transitions (i.e. inside a
    structurally homogeneous region) are merged into a single larger chunk,
    down to *min_size*.

    The resulting boundary list partitions *data* into variable-size chunks
    that each span a structurally uniform region.  Within a static video
    scene, all frames share the same fingerprint class and merge into one
    large chunk.  A scene cut produces a boundary.

    This is most useful for heterogeneous files (mixed-class binary formats,
    long videos with varied content) where fixed-size chunks produce many
    structurally identical consecutive chunks that can be batch-encoded with
    the same bit target.

    Parameters
    ----------
    data            : full file byte data
    initial_size    : coarse chunk size for the first pass (bytes)
    halt_depth_delta: minimum halt_depth change to trigger a boundary
    min_size        : minimum merged chunk size (prevents over-splitting)

    Returns
    -------
    list[int]: boundary byte offsets (including 0 and len(data)) in ascending
               order.  len(boundaries) - 1 == number of adaptive chunks.

    Example
    -------
    boundaries = adaptive_chunk_boundaries(data, 4096)
    chunks = [data[boundaries[i]:boundaries[i+1]]
              for i in range(len(boundaries) - 1)]
    """
    coarse = chunk_analyze(data, chunk_size=initial_size)
    transitions = detect_structural_transitions(
        coarse, halt_depth_delta=halt_depth_delta
    )
    transition_offsets = {t["byte_offset"] for t in transitions}

    # Build boundary list: start, every transition offset, end
    boundaries = [0]
    for offset in sorted(transition_offsets):
        # Enforce minimum chunk size
        if offset - boundaries[-1] >= min_size:
            boundaries.append(offset)

    if boundaries[-1] != len(data):
        boundaries.append(len(data))

    return boundaries


# ─────────────────────────────────────────────────────────────────────────────
# 6.  CHUNKED ENCODING — AIMC v1 FORMAT
# ─────────────────────────────────────────────────────────────────────────────
#
# Format specification:
#
#   Global header (8 bytes):
#     [4]  magic      = b'AIMC'
#     [4]  n_chunks   = uint32 big-endian
#
#   Per-chunk record:
#     [1]  target_bit  uint8   0–7: bit used for L0 clearing
#                              0xFF: chunk was stored as raw gzip (AIM skipped)
#     [4]  orig_size   uint32 big-endian: original chunk length in bytes
#     [4]  enc_size    uint32 big-endian: encoded payload length in bytes
#     [enc_size] payload
#
#   Payload (target_bit 0–7):
#     [4]  aligned_len  uint32 big-endian: length of aligned byte stream
#     [aligned_len]     aligned values (raw bytes, each 0–255)
#     [remaining]       gzip-compressed flag-position encoding
#                       (bitset-vs-delta, same selection as _encode_flags_raw)
#
#   Payload (target_bit 0xFF — raw gzip):
#     [enc_size]        gzip of original chunk bytes
#
# Rationale for the two-path design:
#   Chunks classified as "Uniform noise / encrypted" produce L0 flag counts
#   near 50 %, meaning the aligned stream and flag stream together are no
#   smaller than the original before compression.  Applying AIM adds ~32-byte
#   overhead per chunk (header cost) with no structural benefit.  The raw gzip
#   path avoids this, and its sentinel value (0xFF) makes the skip transparent
#   to the decoder.  See N3 / NOAA experiment for the empirical basis.

_AIMC_MAGIC = b"AIMC"
_AIMC_VERSION = 1
_SKIP_SENTINEL = 0xFF


def _is_noise_class(fingerprint: Optional[Dict]) -> bool:
    """Return True if the fingerprint is classified as noise or encrypted."""
    if fingerprint is None:
        return False
    cls = fingerprint.get("cls", "")
    return "noise" in cls.lower() or "encrypted" in cls.lower()


def _encode_chunk(chunk: List[int], target_bit: int,
                  gz_level: int = 9) -> bytes:
    """
    Encode a single chunk using L0 bit-clearing at *target_bit*.

    Matches Mode 3 (targeted split) from compute_compression_benchmark():
      Aligned stream  -> gzip
      Flag positions  -> _encode_flags_targeted() best of {bitset-raw,
                         delta-raw, bitset+gzip, delta+gzip}

    Payload layout:
      [4]               aligned_gz_len  uint32 big-endian
      [aligned_gz_len]  gzip of aligned byte values
      [remaining]       flag encoding (output of _encode_flags_targeted)
    """
    from aim_core_v3 import _encode_flags_targeted

    aligned, flags = bit_clear_pass(chunk, target_bit)
    aligned_gz     = _gz(bytes(aligned), gz_level)
    flag_payload, _method = _encode_flags_targeted(flags, len(chunk), gz_level)

    return struct.pack(">I", len(aligned_gz)) + aligned_gz + flag_payload


def _decode_chunk(payload: bytes, orig_size: int, target_bit: int) -> List[int]:
    """
    Decode a single AIMC payload back to the original byte list.

    Inverse of _encode_chunk.  Reads:
      [4]               aligned_gz_len
      [aligned_gz_len]  gzip of aligned bytes -> decompress -> orig_size bytes
      [remaining]       flag encoding (bitset or delta, with or without gzip wrapper)
    """
    aligned_gz_len = struct.unpack(">I", payload[:4])[0]
    aligned        = list(gzip.decompress(payload[4 : 4 + aligned_gz_len]))
    flag_payload   = payload[4 + aligned_gz_len :]

    if flag_payload:
        # Detect gzip wrapper by magic bytes (0x1f 0x8b)
        if len(flag_payload) >= 2 and flag_payload[0] == 0x1f and flag_payload[1] == 0x8b:
            flag_raw = gzip.decompress(flag_payload)
        else:
            flag_raw = flag_payload

        # Distinguish bitset from delta by checking expected bitset length
        bitset_len = math.ceil(orig_size / 8)
        if len(flag_raw) == bitset_len:
            flags = []
            for byte_i, byte_val in enumerate(flag_raw):
                for bit in range(8):
                    if byte_val & (1 << bit):
                        idx = byte_i * 8 + bit
                        if idx < orig_size:
                            flags.append(idx)
        else:
            # Delta encoding: 4-byte big-endian count, then count × (1 or 2) byte deltas
            count      = struct.unpack(">I", flag_raw[:4])[0]
            body       = flag_raw[4:]
            bytes_each = len(body) // count if count > 0 else 1
            deltas     = []
            for i in range(count):
                seg = body[i * bytes_each : (i + 1) * bytes_each]
                deltas.append(struct.unpack(">H", seg)[0] if bytes_each == 2 else seg[0])
            flags = []
            pos = 0
            for d in deltas:
                pos += d
                flags.append(pos)
    else:
        flags = []

    mask = 1 << target_bit
    for idx in flags:
        aligned[idx] |= mask

    return aligned


def chunked_encode(
    data: List[int],
    chunk_size: Optional[int] = None,
    *,
    frame_size: Optional[int] = None,
    width: Optional[int] = None,
    height: Optional[int] = None,
    channels: int = 3,
    target_chunks: int = 16,
    skip_noise: bool = True,
    gz_level: int = 9,
) -> bytes:
    """
    Encode *data* chunk-by-chunk using per-chunk adaptive bit selection.

    Each chunk is independently analysed to determine the optimal L0 bit
    target.  Chunks classified as "Uniform noise / encrypted" are encoded as
    raw gzip when *skip_noise* is True, avoiding AIM overhead for regions
    where no structural gain is expected.

    This is the primary encoding path for large binary files and video.  The
    output is a self-describing AIMC v1 byte stream, decodable by
    chunked_decode() without any external metadata.

    Parameters
    ----------
    data          : list[int]   original file as byte values (0–255)
    chunk_size    : int|None    fixed-size chunking (bytes per chunk)
    frame_size    : int|None    video frame-aligned chunking
    width, height : int|None    video frame dimensions (alternative to frame_size)
    channels      : int         bytes per pixel (default 3)
    target_chunks : int         chunk count hint for adaptive sizing
    skip_noise    : bool        skip AIM on noise-class chunks (default True)
    gz_level      : int         gzip compression level 1–9 (default 9)

    Returns
    -------
    bytes: complete AIMC v1 encoded stream
    """
    # Determine chunks
    if width is not None and height is not None:
        raw_chunks = video_frame_chunks(data, frame_size or 0,
                                        width=width, height=height,
                                        channels=channels)
    elif frame_size is not None:
        raw_chunks = video_frame_chunks(data, frame_size)
    else:
        cs = chunk_size or adaptive_chunk_size(len(data), target_chunks)
        raw_chunks = fixed_size_chunks(data, cs)

    chunk_payloads: List[Tuple[int, int, bytes]] = []  # (target_bit, orig_size, payload)

    for _offset, chunk in raw_chunks:
        orig_size = len(chunk)
        analysis  = lightweight_analyze(chunk)
        fp        = analysis["fingerprint"]

        if skip_noise and _is_noise_class(fp):
            # Raw gzip path — AIM offers no benefit here
            payload    = _gz(bytes(chunk), gz_level)
            target_bit = _SKIP_SENTINEL
        else:
            target_bit = analysis["primary_target_bit"]
            payload    = _encode_chunk(chunk, target_bit, gz_level)

        chunk_payloads.append((target_bit, orig_size, payload))

    # Assemble AIMC stream
    parts: List[bytes] = [_AIMC_MAGIC, struct.pack(">I", len(chunk_payloads))]
    for target_bit, orig_size, payload in chunk_payloads:
        parts.append(struct.pack(">B", target_bit))
        parts.append(struct.pack(">I", orig_size))
        parts.append(struct.pack(">I", len(payload)))
        parts.append(payload)

    return b"".join(parts)


def chunked_decode(encoded: bytes) -> List[int]:
    """
    Decode an AIMC v1 byte stream back to the original data.

    Inverse of chunked_encode().  Requires no external metadata — all chunk
    boundaries, bit targets, and sizes are recorded in the stream header.

    Parameters
    ----------
    encoded : bytes   AIMC v1 stream from chunked_encode()

    Returns
    -------
    list[int]: reconstructed original data as byte values (0–255)
    """
    if encoded[:4] != _AIMC_MAGIC:
        raise ValueError(f"Not an AIMC stream (expected magic {_AIMC_MAGIC!r})")

    n_chunks = struct.unpack(">I", encoded[4:8])[0]
    pos      = 8
    result   = []

    for _ in range(n_chunks):
        target_bit = struct.unpack(">B", encoded[pos : pos + 1])[0]; pos += 1
        orig_size  = struct.unpack(">I", encoded[pos : pos + 4])[0]; pos += 4
        enc_size   = struct.unpack(">I", encoded[pos : pos + 4])[0]; pos += 4
        payload    = encoded[pos : pos + enc_size];                   pos += enc_size

        if target_bit == _SKIP_SENTINEL:
            result.extend(gzip.decompress(payload))
        else:
            result.extend(_decode_chunk(payload, orig_size, target_bit))

    return result


# ─────────────────────────────────────────────────────────────────────────────
# 7.  COMPARISON UTILITY — chunked vs global encoding
# ─────────────────────────────────────────────────────────────────────────────

def compare_global_vs_chunked(
    data: List[int],
    chunk_size: Optional[int] = None,
    *,
    frame_size: Optional[int] = None,
    gz_level: int = 9,
) -> Dict:
    """
    Compare global AIM encoding against chunked AIM encoding on the same data.

    Runs three encodings:
      1. Raw gzip (baseline)
      2. Global bit-clear + gzip (single optimal bit for whole file, then gzip)
      3. Chunked AIMC (per-chunk optimal bit, skip_noise=True)

    This is the primary empirical tool for evaluating whether chunking adds
    value over a single-pass global encoding for a given file.

    The expected pattern from the experimental program:
      • Homogeneous files (single fingerprint class throughout): global ≈ chunked.
        Chunked adds small overhead from per-chunk headers.
      • Heterogeneous files (mixed structure across regions): chunked < global.
        The improvement scales with structural variance across chunks.
      • High-noise files: chunked ≤ raw gzip.  skip_noise=True avoids AIM
        overhead; remaining structured chunks may show marginal improvement.

    Returns
    -------
    dict:
      original_bytes     – len(data)
      raw_gzip_bytes     – gzip(data) size
      global_aim_bytes   – single-pass L0 clear + gzip of aligned + gzip of flags
      chunked_aim_bytes  – AIMC encoded size
      raw_gzip_ratio     – raw_gzip / original
      global_aim_ratio   – global_aim / original
      chunked_aim_ratio  – chunked_aim / original
      chunked_vs_global_delta_pct – (chunked - global) / original × 100
        (negative means chunked is smaller)
      n_skip_chunks      – count of chunks that used raw gzip path
      n_aim_chunks       – count of chunks that used AIM path
      n_total_chunks     – total chunk count
    """
    from aim_core_v3 import (
        bit_clear_pass as _bcp,
        _encode_as_bitset as _ebs,   # used for safe global flag sizing  # noqa: F401
        _gz as _gz_core,
        bit_clear_sweep as _bcs,
    )

    original_bytes = len(data)

    # 1. Raw gzip
    raw_gzip_bytes = len(_gz_core(bytes(data), gz_level))

    # 2. Global AIM: find single best bit, clear, gzip both streams.
    #    Use bitset encoding for flags to avoid delta-encoding overflow on
    #    large sparse flag sets (delta values can exceed uint16 range when
    #    original_bytes > 65535 and flags are sparse).  Bitset is a safe
    #    upper bound; delta would only ever be smaller, so this gives a
    #    conservative (unfavourable-to-global) estimate in the comparison.
    from aim_core_v3 import _encode_as_bitset as _ebs
    sweep = _bcs(data)
    best_bit = min(sweep, key=lambda r: (r["total"], r["bit"]))["bit"]
    aligned_g, flags_g = _bcp(data, best_bit)
    flags_bitset_g = _ebs(flags_g, original_bytes)
    global_aim_bytes = (
        len(_gz_core(bytes(aligned_g), gz_level))
        + len(_gz_core(flags_bitset_g, gz_level))
    )

    # 3. Chunked AIMC
    cs = chunk_size or adaptive_chunk_size(len(data))
    fs = frame_size

    if fs is not None:
        encoded = chunked_encode(data, frame_size=fs, gz_level=gz_level)
    else:
        encoded = chunked_encode(data, chunk_size=cs, gz_level=gz_level)

    chunked_aim_bytes = len(encoded)

    # Count skip vs AIM chunks from the encoded stream
    n_skip = 0
    n_aim  = 0
    n_total_chunks = struct.unpack(">I", encoded[4:8])[0]
    pos = 8
    for _ in range(n_total_chunks):
        tb = encoded[pos]; pos += 1
        pos += 4  # orig_size
        enc_sz = struct.unpack(">I", encoded[pos : pos + 4])[0]; pos += 4
        pos += enc_sz
        if tb == _SKIP_SENTINEL:
            n_skip += 1
        else:
            n_aim += 1

    def _ratio(num: int) -> float:
        return round(num / original_bytes, 6) if original_bytes else 0.0

    return {
        "original_bytes":              original_bytes,
        "raw_gzip_bytes":              raw_gzip_bytes,
        "global_aim_bytes":            global_aim_bytes,
        "chunked_aim_bytes":           chunked_aim_bytes,
        "raw_gzip_ratio":              _ratio(raw_gzip_bytes),
        "global_aim_ratio":            _ratio(global_aim_bytes),
        "chunked_aim_ratio":           _ratio(chunked_aim_bytes),
        "chunked_vs_global_delta_pct": round(
            (chunked_aim_bytes - global_aim_bytes) / original_bytes * 100, 4
        ),
        "n_skip_chunks":               n_skip,
        "n_aim_chunks":                n_aim,
        "n_total_chunks":              n_total_chunks,
    }


# ─────────────────────────────────────────────────────────────────────────────
# 8.  DEMO — illustrative run on synthetic video-like data
# ─────────────────────────────────────────────────────────────────────────────

def _demo_synthetic_video(
    n_frames: int = 20,
    frame_width: int = 64,
    frame_height: int = 64,
    static_fraction: float = 0.4,
    seed: int = 42,
) -> List[int]:
    """
    Generate a synthetic raw-RGB video byte stream for demo purposes.

    Produces *n_frames* frames at *frame_width* × *frame_height* pixels (RGB).
    The first *static_fraction* of frames are gradient-ramp data (structured).
    The remaining frames are uniform random (noise-class).

    This models a video opening with a static title card followed by
    high-motion content — the canonical heterogeneous case where chunked
    encoding is expected to outperform global encoding.
    """
    import random
    rng = random.Random(seed)
    frame_size = frame_width * frame_height * 3
    frames = []

    n_static = int(n_frames * static_fraction)
    for i in range(n_frames):
        if i < n_static:
            # Gradient-class: linear ramp per channel, mostly even values
            frame = [int(255 * j / frame_size) & 0xFE
                     for j in range(frame_size)]
        else:
            # Noise-class: uniform random
            frame = [rng.randint(0, 255) for _ in range(frame_size)]
        frames.extend(frame)

    return frames


def run_demo() -> None:
    """
    Run the chunking demo on synthetic video data and print a summary.

    Two scenarios are shown:

    Scenario A — "Static title card then high-motion" (class-level heterogeneity)
      Gradient-class (all-even ramp) frames followed by noise-class frames.
      Demonstrates structural transition detection and the AIM-skip decision.
      Chunking overhead is small (+0.3%) because the structured frames are
      trivially compressible already and noise frames go through raw gzip.

    Scenario B — "Multi-scene structured video" (bit-target heterogeneity)
      Half of the frames use a prime-gap byte distribution (bit 4 optimal).
      Half use a Fibonacci byte distribution (bit 6 optimal).
      Global AIM is forced to pick one global bit (a compromise).
      Chunked AIM picks the optimal bit independently per frame.
      This is the scenario where chunked encoding is expected to win.
    """
    from aim_core_v3 import gen_prime_gaps, gen_fibonacci

    # ── Scenario A ──────────────────────────────────────────────────────────
    print("Scenario A — Class-level heterogeneity (gradient + noise frames)")
    print("=" * 60)

    data_a     = _demo_synthetic_video(n_frames=20, frame_width=64,
                                       frame_height=64, static_fraction=0.4)
    frame_size = 64 * 64 * 3

    print(f"Total data: {len(data_a):,} bytes  |  Frame size: {frame_size:,} bytes")
    results_a = chunk_analyze(data_a, frame_size=frame_size)
    series_a  = complexity_series(results_a)

    print(f"\nFrame-level complexity series:")
    print(f"  {'#':>3}  {'class':<30}  {'depth':>5}  {'bit':>3}  {'entropy%':>9}")
    for row in series_a:
        print(f"  {row['index']:>3}  {row['cls']:<30}  {row['halt_depth']:>5}"
              f"  {row['primary_target_bit']:>3}  {row['best_entropy_pct']:>+9.2f}%")

    transitions_a = detect_structural_transitions(results_a)
    print(f"\nDetected structural transitions: {len(transitions_a)}")
    for t in transitions_a:
        print(f"  Frames {t['between_chunks'][0]}→{t['between_chunks'][1]}"
              f"  byte {t['byte_offset']:,}: {'; '.join(t['reasons'])}")

    summary_a = summarize_chunked_analysis(results_a)
    print(f"\n  AIM-skip rate (noise chunks): {summary_a['compression_skip_pct']:.1f}%"
          f"  ({summary_a['n_noise_chunks']}/{summary_a['n_chunks']} frames)")

    cmp_a = compare_global_vs_chunked(data_a, frame_size=frame_size)
    print(f"\nGlobal vs Chunked encoding (Scenario A):")
    print(f"  Raw gzip      {cmp_a['raw_gzip_bytes']:>8,} bytes  "
          f"({cmp_a['raw_gzip_ratio']:.4f})")
    print(f"  Global AIM    {cmp_a['global_aim_bytes']:>8,} bytes  "
          f"({cmp_a['global_aim_ratio']:.4f})")
    print(f"  Chunked AIMC  {cmp_a['chunked_aim_bytes']:>8,} bytes  "
          f"({cmp_a['chunked_aim_ratio']:.4f})"
          f"  Δ={cmp_a['chunked_vs_global_delta_pct']:+.2f}%")

    # ── Scenario B ──────────────────────────────────────────────────────────
    print()
    print("Scenario B — Bit-target heterogeneity (prime gaps + Fibonacci frames)")
    print("=" * 60)

    n_frames_b  = 16
    frame_b     = 4096          # bytes per frame
    half        = n_frames_b // 2

    # Prime gaps: bit 4 is the entropy winner (−11% reduction, confirmed N6)
    prime_chunk = gen_prime_gaps(frame_b)
    # Fibonacci:  bit 6 is the entropy winner (−7% reduction, confirmed V2)
    fib_chunk   = gen_fibonacci(frame_b)

    data_b = []
    for i in range(n_frames_b):
        data_b.extend(prime_chunk if i < half else fib_chunk)

    print(f"Total data: {len(data_b):,} bytes  |  Frame size: {frame_b:,} bytes")
    print(f"Frames 0–{half-1}: prime gaps  (bit-4 optimal)")
    print(f"Frames {half}–{n_frames_b-1}: Fibonacci    (bit-6 optimal)")

    results_b = chunk_analyze(data_b, frame_size=frame_b)
    series_b  = complexity_series(results_b)

    print(f"\nFrame-level complexity series:")
    print(f"  {'#':>3}  {'class':<24}  {'depth':>5}  {'sweep bit':>9}  {'entropy%':>9}")
    for row in series_b:
        print(f"  {row['index']:>3}  {row['cls']:<24}  {row['halt_depth']:>5}"
              f"  {row['primary_target_bit']:>9}  {row['best_entropy_pct']:>+9.2f}%")

    transitions_b = detect_structural_transitions(results_b)
    print(f"\nDetected structural transitions: {len(transitions_b)}")
    for t in transitions_b:
        print(f"  Frames {t['between_chunks'][0]}→{t['between_chunks'][1]}"
              f"  byte {t['byte_offset']:,}: {'; '.join(t['reasons'])}")

    cmp_b = compare_global_vs_chunked(data_b, frame_size=frame_b)
    print(f"\nGlobal vs Chunked encoding (Scenario B):")
    print(f"  Raw gzip      {cmp_b['raw_gzip_bytes']:>8,} bytes  "
          f"({cmp_b['raw_gzip_ratio']:.4f})")
    print(f"  Global AIM    {cmp_b['global_aim_bytes']:>8,} bytes  "
          f"({cmp_b['global_aim_ratio']:.4f})")
    print(f"  Chunked AIMC  {cmp_b['chunked_aim_bytes']:>8,} bytes  "
          f"({cmp_b['chunked_aim_ratio']:.4f})"
          f"  Δ={cmp_b['chunked_vs_global_delta_pct']:+.2f}%")

    # ── Round-trip ──────────────────────────────────────────────────────────
    print()
    print("Round-trip integrity check:")
    for label, data, fs in [("Scenario A", data_a, frame_size),
                             ("Scenario B", data_b, frame_b)]:
        encoded   = chunked_encode(data, frame_size=fs)
        recovered = chunked_decode(encoded)
        ok = recovered == data
        n_diff = sum(1 for a, b in zip(data, recovered) if a != b) if not ok else 0
        print(f"  {label}: decode(encode(data)) == data : "
              f"{'✓ PASS' if ok else f'✗ FAIL ({n_diff} bytes differ)'}")


def write_frame_log(result: Dict, log_dir: str) -> None:
    """
    Write a detailed per-frame/chunk analysis log to *log_dir*.

    Filename: ``frame_{index:06d}.txt``

    Contents
    --------
    Header
      Frame index, byte offset, size, fingerprint class, halt depth,
      L0 odd ratio, L0 bit distribution, sweep winner, entropy winner.

    REAL Decay Profile
      Full layer-by-layer table: depth, input size, odd count, odd ratio,
      bit distribution across all 8 bit positions.

    Bit-Clear Sweep
      For each of the 8 bit positions: total flags, per-layer flag count and
      flag ratio.

    Per-Bit Entropy Table
      For each of the 8 bit positions: aligned entropy, aligned bits,
      flag bits, output bits, net bits, net %, verdict.

    Fingerprint
      Classification label, description, similarity scores against all
      reference profiles.

    Byte-Alignment Invariant (if chunk length is a multiple of 8)
      Predicted terminal halt value and whether it matches the actual result.
    """
    import pathlib
    from aim_core_v3 import compute_invariant

    pathlib.Path(log_dir).mkdir(parents=True, exist_ok=True)
    idx      = result["index"]
    offset   = result["offset"]
    size     = result["size"]
    fp       = result.get("fingerprint") or {}
    layers   = result.get("decay_layers", [])
    sweep    = result.get("bit_sweep", [])
    entropies = result.get("all_bit_entropies", [])
    ptb      = result.get("primary_target_bit", "?")
    beb      = result.get("best_entropy_bit", "?")
    bep      = result.get("best_entropy_pct", 0.0)

    lines = []
    W = 72  # line width for section separators

    def sep(title=""):
        if title:
            pad = W - len(title) - 4
            lines.append(f"── {title} {'─' * max(0, pad)}")
        else:
            lines.append("─" * W)

    # ── Header ────────────────────────────────────────────────────────────────
    sep("FRAME ANALYSIS")
    lines.append(f"  Frame index      : {idx}")
    lines.append(f"  Byte offset      : {offset:,}")
    lines.append(f"  Chunk size       : {size:,} bytes")
    lines.append(f"  Class            : {fp.get('cls', 'unknown')}")
    lines.append(f"  Halt depth       : {fp.get('halt_depth', '?')}")
    lines.append(f"  L0 odd ratio     : {fp.get('l0_odd_ratio', '?')}")
    lines.append(f"  L0 stddev        : {fp.get('stddev', '?')}")
    bd = fp.get("bit_dist", [])
    if bd:
        lines.append(f"  L0 bit dist      : " +
                     "  ".join(f"b{b}={v:.4f}" for b, v in enumerate(bd)))
    lines.append(f"  Sweep winner     : bit {ptb}")
    lines.append(f"  Entropy winner   : bit {beb}  ({bep:+.4f}%)")
    lines.append("")

    # ── REAL Decay Profile ────────────────────────────────────────────────────
    sep("REAL DECAY PROFILE  (bit-0 / even-alignment)")
    if layers:
        # Header row
        lines.append(f"  {'L':>3}  {'size':>8}  {'odd':>8}  {'ratio':>6}  "
                     f"{'b0':>6}  {'b1':>6}  {'b2':>6}  {'b3':>6}  "
                     f"{'b4':>6}  {'b5':>6}  {'b6':>6}  {'b7':>6}")
        lines.append("  " + "─" * 68)
        for L in layers:
            bd_row = L.get("bit_dist", [0.0] * 8)
            lines.append(
                f"  {L['depth']:>3}  {L['size']:>8,}  {L['odd_count']:>8,}  "
                f"{L['odd_ratio']:>6.4f}  " +
                "  ".join(f"{v:>6.4f}" for v in bd_row)
            )
    else:
        lines.append("  (no layers — empty chunk)")
    lines.append("")

    # ── Bit-Clear Sweep ───────────────────────────────────────────────────────
    sep("BIT-CLEAR SWEEP  (total flags per bit target)")
    if sweep:
        lines.append(f"  {'bit':>3}  {'total flags':>12}  layer detail")
        lines.append("  " + "─" * 60)
        for entry in sweep:
            b     = entry["bit"]
            total = entry["total"]
            marker = " ◀ sweep winner" if b == ptb else ""
            layer_summary = "  ".join(
                f"L{l['depth']}:{l['flag_count']}" for l in entry["layers"]
            )
            lines.append(f"  {b:>3}  {total:>12,}  {layer_summary}{marker}")
    else:
        lines.append("  (sweep data unavailable)")
    lines.append("")

    # ── Per-Bit Entropy Table ─────────────────────────────────────────────────
    sep("PER-BIT ENTROPY TABLE")
    if entropies:
        lines.append(f"  {'bit':>3}  {'flag_ratio':>10}  {'flag_count':>10}  "
                     f"{'aln_h':>7}  {'aln_bits':>10}  {'flag_bits':>10}  "
                     f"{'net_bits':>10}  {'net_%':>8}  {'verdict':<12}")
        lines.append("  " + "─" * 90)
        for e in entropies:
            b       = e["bit"]
            marker  = " ◀" if b == beb else ""
            lines.append(
                f"  {b:>3}  {e['flag_ratio']:>10.4f}  {e['flag_count']:>10,}  "
                f"{e['aligned_h']:>7.5f}  {e['aligned_bits']:>10.2f}  "
                f"{e['layer_flag_bits']:>10.2f}  {e['net_bits']:>10.2f}  "
                f"{e['net_pct']:>+8.4f}%  {e['verdict']:<12}{marker}"
            )
    else:
        lines.append("  (entropy data unavailable)")
    lines.append("")

    # ── Flag Position Profile ─────────────────────────────────────────────────
    sep("FLAG POSITION PROFILE  (L0, entropy-winner bit)")
    efp = result.get("entropy_flag_profile", {})
    sfp = result.get("sweep_flag_profile", {})

    def _fmt_profile(p: Dict, label: str, target_bit: int) -> None:
        if not p:
            lines.append(f"  ({label} profile unavailable)")
            return
        lines.append(f"  [{label}  bit {target_bit}]")
        lines.append(f"  Density          : {p['k']:,} / {p['n']:,}  ({p['density']*100:.2f}%)")
        lines.append(f"  Gap min/max/mean : {p['gap_min']} / {p['gap_max']} / {p['gap_mean']:.1f}")
        lines.append(f"  Gap stddev / CV  : {p['gap_stddev']:.2f} / {p['gap_cv']:.4f}")
        lines.append(f"  Gap uniformity   : {p['gap_uniformity']:.4f}  "
                     f"(1=perfectly spaced, 0=maximally bursty)")
        lines.append(f"  Spatial entropy  : {p['spatial_entropy']:.4f} bits  "
                     f"(low=predictable/periodic, high=random)")
        lines.append(f"  Leading gap %    : {p['leading_gap_pct']*100:.2f}%  "
                     f"(adjacent-flag clustering)")
        lines.append(f"  Set runs         : max={p['run_set_max']}  mean={p['run_set_mean']:.1f}")
        lines.append(f"  Clear runs       : max={p['run_clear_max']}  mean={p['run_clear_mean']:.1f}")
        lines.append(f"")
        lines.append(f"  Encoding sizes (bytes, no framing):")
        lines.append(f"    bitset-raw  : {p['enc_bitset']:>10,}")
        lines.append(f"    delta-raw   : {p['enc_delta_raw']:>10,}")
        lines.append(f"    bitset+gzip : {p['enc_bitset_gz']:>10,}")
        lines.append(f"    delta+gzip  : {p['enc_delta_gz']:>10,}")
        winner_marker = lambda m: "  ◀ winner" if m == p['enc_best_method'] else ""
        lines.append(f"    best        : {p['enc_best']:>10,}  [{p['enc_best_method']}]{winner_marker(p['enc_best_method'])}")
        lines.append(f"    vs bitset   : {p['enc_vs_raw_pct']:.1f}% of baseline")

    _fmt_profile(efp, "entropy winner", beb)
    if beb != ptb:
        lines.append("")
        _fmt_profile(sfp, "sweep winner", ptb)
    else:
        lines.append(f"\n  (sweep winner = entropy winner, profile identical)")

    # ── Per-Layer Encoding Breakdown ──────────────────────────────────────────
    sep("PER-LAYER ENCODING BREAKDOWN  (entropy-winner bit, all recursive layers)")
    layer_profiles = result.get("layer_profiles", [])
    if layer_profiles:
        lines.append(f"  {'L':>3}  {'k':>8}  {'density':>8}  {'gap_cv':>7}  "
                     f"{'uniformity':>10}  {'sp_entropy':>10}  "
                     f"{'best_bytes':>10}  {'method':<14}")
        lines.append("  " + "─" * 80)
        for lp in layer_profiles:
            lines.append(
                f"  {lp.get('depth',0):>3}  {lp['k']:>8,}  "
                f"{lp['density']:>8.4f}  {lp['gap_cv']:>7.4f}  "
                f"{lp['gap_uniformity']:>10.4f}  {lp['spatial_entropy']:>10.4f}  "
                f"{lp['enc_best']:>10,}  {lp['enc_best_method']:<14}"
            )
    else:
        lines.append("  (no layer profiles — single-layer chunk)")
    lines.append("")

    # ── Fingerprint ───────────────────────────────────────────────────────────
    sep("FINGERPRINT CLASSIFICATION")
    if fp:
        lines.append(f"  Class       : {fp.get('cls', '?')}")
        lines.append(f"  Description : {fp.get('cls_desc', '?')}")
        lines.append(f"  Use cases   : {', '.join(fp.get('use_cases', []))}")
        lines.append("")
        lines.append("  Reference profile similarity scores:")
        for ref in fp.get("scored_refs", [])[:6]:
            lines.append(f"    {ref['sim']:>6.4f}  {ref['name']}")
    else:
        lines.append("  (fingerprint unavailable)")
    lines.append("")

    # ── Byte-Alignment Invariant ──────────────────────────────────────────────
    sep("BYTE-ALIGNMENT INVARIANT")
    if size % 8 == 0 and layers:
        # Reconstruct minimal data proxy — we don't have raw bytes here,
        # but we can derive the invariant from decay_layers alone.
        inv = compute_invariant([], decay_layers=layers)
        applicable = inv.get("applicable", False)
        if applicable:
            predicted = inv.get("predicted_terminal")
            actual    = inv.get("actual_terminal")
            match     = inv.get("match")
            lines.append(f"  Chunk length {size:,} is a multiple of 8 — invariant applies.")
            lines.append(f"  Predicted terminal : {predicted}")
            lines.append(f"  Actual terminal    : {actual}")
            lines.append(f"  Match              : {'✓ PASS' if match else '✗ FAIL'}")
        else:
            lines.append(f"  Chunk length {size:,} is a multiple of 8 but invariant")
            lines.append(f"  could not be computed from decay layers alone.")
    elif size % 8 != 0:
        lines.append(f"  Chunk length {size:,} is not a multiple of 8 — invariant does not apply.")
    else:
        lines.append("  (no decay layers — invariant skipped)")
    lines.append("")

    sep()

    # ── Write ─────────────────────────────────────────────────────────────────
    log_path = pathlib.Path(log_dir) / f"frame_{idx:06d}.txt"
    log_path.write_text("\n".join(lines), encoding="utf-8")


def _build_arg_parser() -> "argparse.ArgumentParser":
    import argparse

    p = argparse.ArgumentParser(
        prog="aim_chunking",
        description=(
            "AIM Chunking — chunk-level structural analysis and adaptive encoding.\n"
            "Run without arguments to execute the built-in synthetic demo."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples
--------
  # Demo (no args)
  python aim_chunking.py

  # Analyse a binary file with adaptive chunk sizing
  python aim_chunking.py myfile.bin

  # Analyse with a fixed chunk size
  python aim_chunking.py myfile.bin --chunk-size 4096

  # Analyse as raw video frames (width × height × channels)
  python aim_chunking.py video.raw --width 1920 --height 1080

  # Analyse with an explicit frame size in bytes
  python aim_chunking.py video.raw --frame-size 12288

  # Full comparison: raw gzip vs global AIM vs chunked AIM
  python aim_chunking.py myfile.bin --compare

  # Encode to an AIMC file
  python aim_chunking.py myfile.bin --encode output.aimc

  # Decode an AIMC file
  python aim_chunking.py output.aimc --decode recovered.bin

  # Write per-frame detail logs to a folder
  python aim_chunking.py video.raw --frame-size 2304000 --log-dir ./frame_logs

  # Full run: analysis + summary + transitions + per-frame logs
  python aim_chunking.py video.raw --frame-size 2304000 --summary --transitions --log-dir ./logs
""",
    )

    p.add_argument(
        "file",
        nargs="?",
        metavar="FILE",
        help="Input file to analyse/encode/decode. Omit to run the built-in demo.",
    )

    # ── Chunking strategy ────────────────────────────────────────────────────
    chunk_grp = p.add_argument_group("chunking strategy (mutually exclusive)")
    mx = chunk_grp.add_mutually_exclusive_group()
    mx.add_argument(
        "--chunk-size", type=int, metavar="BYTES",
        help="Fixed chunk size in bytes.",
    )
    mx.add_argument(
        "--frame-size", type=int, metavar="BYTES",
        help="Video frame size in bytes (frame-aligned chunking).",
    )
    mx.add_argument(
        "--width", type=int, metavar="PX",
        help="Frame width in pixels (use with --height; implies frame-aligned chunking).",
    )
    chunk_grp.add_argument(
        "--height", type=int, metavar="PX",
        help="Frame height in pixels (use with --width).",
    )
    chunk_grp.add_argument(
        "--channels", type=int, default=3, metavar="N",
        help="Bytes per pixel for video mode (default: 3 = RGB).",
    )
    chunk_grp.add_argument(
        "--target-chunks", type=int, default=16, metavar="N",
        help="Target number of chunks for adaptive sizing (default: 16).",
    )

    # ── Actions ──────────────────────────────────────────────────────────────
    act_grp = p.add_argument_group("actions")
    act_grp.add_argument(
        "--compare", action="store_true",
        help="Print a global-vs-chunked compression comparison table.",
    )
    act_grp.add_argument(
        "--encode", metavar="OUT_FILE",
        help="Encode input to AIMC format and write to OUT_FILE.",
    )
    act_grp.add_argument(
        "--decode", metavar="OUT_FILE",
        help="Decode an AIMC-encoded FILE and write recovered bytes to OUT_FILE.",
    )
    act_grp.add_argument(
        "--transitions", action="store_true",
        help="Detect and print structural transitions between chunks.",
    )
    act_grp.add_argument(
        "--summary", action="store_true",
        help="Print aggregate statistics over all chunks.",
    )

    p.add_argument(
        "--gz-level", type=int, default=9, metavar="1-9",
        help="gzip compression level (default: 9).",
    )
    p.add_argument(
        "--workers", type=int, default=None, metavar="N",
        help="Worker processes for parallel chunk analysis "
             "(default: os.cpu_count(); pass 1 to run single-process).",
    )
    p.add_argument(
        "--log-dir", metavar="DIR",
        help="Write one detailed .txt log per chunk/frame into DIR. "
             "Directory is created if it does not exist. "
             "Each log contains the full REAL decay profile, bit-clear sweep, "
             "per-bit entropy table, fingerprint classification, and byte-alignment "
             "invariant (where applicable).",
    )
    p.add_argument(
        "--skip-rle", action="store_true",
        help="Skip run-length analysis in flag position profiles (faster for large files). "
             "Gap, encoding size, and clustering stats are still computed; only "
             "set/clear run-length fields are omitted.",
    )

    return p


class _Tee:
    """
    Mirrors all writes to *stdout* into an open file handle simultaneously.

    Usage::

        with open(path, "w") as fh:
            with _Tee(sys.stdout, fh) as tee:
                sys.stdout = tee
                ...
        sys.stdout = tee.primary   # restored automatically on __exit__

    Works with print(), tqdm (which writes to sys.stderr separately), and any
    code that calls sys.stdout.write() directly.
    """

    def __init__(self, primary, secondary):
        self.primary   = primary
        self.secondary = secondary

    # ── file-like interface ───────────────────────────────────────────────────
    def write(self, data: str) -> int:
        self.secondary.write(data)
        return self.primary.write(data)

    def flush(self) -> None:
        self.primary.flush()
        self.secondary.flush()

    def fileno(self):           # needed by some stdlib internals
        return self.primary.fileno()

    @property
    def encoding(self):
        return getattr(self.primary, "encoding", "utf-8")

    @property
    def errors(self):
        return getattr(self.primary, "errors", "replace")

    # ── context manager ───────────────────────────────────────────────────────
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.flush()


def _fmt_duration(seconds: float) -> str:
    """Format a duration in seconds as a human-readable string."""
    if seconds < 60:
        return f"{seconds:.1f}s"
    m, s = divmod(int(seconds), 60)
    return f"{m}m{s:02d}s"


def _run_cli(args: "argparse.Namespace") -> None:
    import sys

    phase_times: List[Tuple[str, float]] = []   # (label, elapsed_seconds)

    def _phase(label: str, t0: float) -> float:
        elapsed = time.monotonic() - t0
        phase_times.append((label, elapsed))
        return elapsed

    # ── Load input file ───────────────────────────────────────────────────────
    t0 = time.monotonic()
    try:
        with open(args.file, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        print(f"Error reading '{args.file}': {exc}", file=sys.stderr)
        sys.exit(1)
    data = list(raw)
    load_s = _phase("Load", t0)
    print(f"Loaded '{args.file}': {len(data):,} bytes  [{_fmt_duration(load_s)}]")

    # ── Decode mode ───────────────────────────────────────────────────────────
    if args.decode:
        t0 = time.monotonic()
        recovered = chunked_decode(bytes(data))
        try:
            with open(args.decode, "wb") as fh:
                fh.write(bytes(recovered))
            print(f"Decoded → '{args.decode}' ({len(recovered):,} bytes)"
                  f"  [{_fmt_duration(_phase('Decode', t0))}]")
        except OSError as exc:
            print(f"Error writing '{args.decode}': {exc}", file=sys.stderr)
            sys.exit(1)
        return

    # ── Resolve chunking kwargs ────────────────────────────────────────────────
    chunk_kwargs: dict = {
        "target_chunks": args.target_chunks,
        "channels":      args.channels,
        "workers":       args.workers,
        "skip_rle":      args.skip_rle,
    }
    if args.width and args.height:
        chunk_kwargs["width"]  = args.width
        chunk_kwargs["height"] = args.height
    elif args.frame_size:
        chunk_kwargs["frame_size"] = args.frame_size
    elif args.chunk_size:
        chunk_kwargs["chunk_size"] = args.chunk_size

    # ── Pre-compute chunk count estimate ──────────────────────────────────────
    if args.width and args.height:
        _n_chunks = max(1, len(data) // (args.width * args.height * args.channels))
    elif args.frame_size:
        _n_chunks = max(1, len(data) // args.frame_size)
    elif args.chunk_size:
        _n_chunks = math.ceil(len(data) / args.chunk_size)
    else:
        _n_chunks = chunk_kwargs.get("target_chunks", 16)

    rle_note = "  [--skip-rle active]" if args.skip_rle else ""
    print(f"\nAnalysing {_n_chunks:,} chunks "
          f"({'tqdm' if _HAS_TQDM else 'built-in progress'}){rle_note}…")

    # ── Analysis phase ────────────────────────────────────────────────────────
    t0 = time.monotonic()
    results = chunk_analyze(data, **chunk_kwargs)
    series  = complexity_series(results)
    analysis_s = _phase("Analysis", t0)
    print(f"  Analysis complete: {len(results)} chunks  [{_fmt_duration(analysis_s)}]"
          f"  ({len(results)/analysis_s:.1f} chunks/s)")

    # ── Optional: per-frame logs ──────────────────────────────────────────────
    if args.log_dir:
        print(f"\nWriting frame logs → '{args.log_dir}' …")
        t0 = time.monotonic()
        for r in _progress_iter(results, total=len(results), desc="Writing logs"):
            write_frame_log(r, args.log_dir)
        log_s = _phase("Log writing", t0)
        print(f"  {len(results)} logs written  [{_fmt_duration(log_s)}]")

    # ── Complexity series ─────────────────────────────────────────────────────
    print(f"\nChunk-level complexity series ({len(series)} chunks):")
    print(f"  {'#':>5}  {'offset':>10}  {'size':>8}  {'class':<30}  "
          f"{'depth':>5}  {'bit':>3}  {'entropy%':>9}")
    for row in series:
        print(f"  {row['index']:>5}  {row['offset']:>10,}  {row['size']:>8,}  "
              f"{row['cls']:<30}  {row['halt_depth']:>5}  "
              f"{row['primary_target_bit']:>3}  {row['best_entropy_pct']:>+9.2f}%")

    # ── Optional: transitions ─────────────────────────────────────────────────
    if args.transitions:
        t0 = time.monotonic()
        transitions = detect_structural_transitions(results)
        trans_s = _phase("Transitions", t0)
        print(f"\nStructural transitions detected: {len(transitions)}"
              f"  [{_fmt_duration(trans_s)}]")
        for t in transitions:
            print(f"  Chunks {t['between_chunks'][0]}→{t['between_chunks'][1]}"
                  f"  byte {t['byte_offset']:,}: {'; '.join(t['reasons'])}")

    # ── Optional: summary ─────────────────────────────────────────────────────
    if args.summary:
        t0 = time.monotonic()
        s = summarize_chunked_analysis(results)
        print(f"\nAggregate summary:  [{_fmt_duration(_phase('Summary', t0))}]")
        print(f"  Chunks          : {s['n_chunks']}")
        print(f"  Total bytes     : {s['total_bytes']:,}")
        print(f"  Halt depth      : mean={s['halt_depth_mean']}"
              f"  min={s['halt_depth_min']}  max={s['halt_depth_max']}")
        print(f"  Reduction chunks: {s['n_reduction_chunks']} "
              f"({100*s['n_reduction_chunks']/max(1,s['n_chunks']):.1f}%)")
        print(f"  Noise/skip      : {s['n_noise_chunks']} "
              f"({s['compression_skip_pct']:.1f}%)")
        print(f"  Class dist      : {s['class_distribution']}")
        print(f"  Bit dist        : {s['bit_distribution']}")

    # ── Optional: compare ─────────────────────────────────────────────────────
    if args.compare:
        t0 = time.monotonic()
        print(f"\nGlobal vs Chunked compression comparison…")
        cmp_kwargs: dict = {"gz_level": args.gz_level}
        if args.frame_size:
            cmp_kwargs["frame_size"] = args.frame_size
        elif args.chunk_size:
            cmp_kwargs["chunk_size"] = args.chunk_size
        cmp = compare_global_vs_chunked(data, **cmp_kwargs)
        cmp_s = _phase("Compare", t0)
        print(f"  Raw gzip      {cmp['raw_gzip_bytes']:>10,} bytes  "
              f"(ratio {cmp['raw_gzip_ratio']:.6f})")
        print(f"  Global AIM    {cmp['global_aim_bytes']:>10,} bytes  "
              f"(ratio {cmp['global_aim_ratio']:.6f})")
        print(f"  Chunked AIMC  {cmp['chunked_aim_bytes']:>10,} bytes  "
              f"(ratio {cmp['chunked_aim_ratio']:.6f})"
              f"  Δ={cmp['chunked_vs_global_delta_pct']:+.4f}%")
        print(f"  Chunks: {cmp['n_total_chunks']} total  "
              f"({cmp['n_aim_chunks']} AIM, {cmp['n_skip_chunks']} raw-gzip skip)"
              f"  [{_fmt_duration(cmp_s)}]")

    # ── Optional: encode ──────────────────────────────────────────────────────
    if args.encode:
        t0 = time.monotonic()
        print(f"\nEncoding → '{args.encode}' …")
        enc_kwargs: dict = {"gz_level": args.gz_level}
        if args.width and args.height:
            enc_kwargs["frame_size"] = args.width * args.height * args.channels
        elif args.frame_size:
            enc_kwargs["frame_size"] = args.frame_size
        elif args.chunk_size:
            enc_kwargs["chunk_size"] = args.chunk_size
        encoded = chunked_encode(data, **enc_kwargs)
        enc_s = _phase("Encode", t0)
        try:
            with open(args.encode, "wb") as fh:
                fh.write(encoded)
            ratio = len(encoded) / max(1, len(data))
            print(f"  Written {len(encoded):,} bytes  "
                  f"(ratio {ratio:.6f})  [{_fmt_duration(enc_s)}]")
        except OSError as exc:
            print(f"Error writing '{args.encode}': {exc}", file=sys.stderr)
            sys.exit(1)

    # ── Phase timing summary ──────────────────────────────────────────────────
    total_s = sum(s for _, s in phase_times)
    print(f"\n── Phase timings {'─' * 40}")
    for label, elapsed in phase_times:
        pct = 100 * elapsed / total_s if total_s > 0 else 0
        bar = "█" * int(pct / 5)
        print(f"  {label:<20} {_fmt_duration(elapsed):>8}  "
              f"({pct:5.1f}%)  {bar}")
    print(f"  {'Total':<20} {_fmt_duration(total_s):>8}")


if __name__ == "__main__":
    import argparse
    import pathlib
    import sys

    parser = _build_arg_parser()
    args   = parser.parse_args()

    if args.file is None:
        run_demo()
    else:
        # ── Open meta log if --log-dir was given ──────────────────────────────
        log_fh   = None
        orig_out = sys.stdout

        if args.log_dir:
            pathlib.Path(args.log_dir).mkdir(parents=True, exist_ok=True)
            log_path = pathlib.Path(args.log_dir) / "MainLog.txt"
            try:
                log_fh     = open(log_path, "w", encoding="utf-8")
                sys.stdout = _Tee(orig_out, log_fh)
                print(f"# aim_chunking  —  meta log")
                print(f"# Input  : {args.file}")
                print(f"# Log dir: {args.log_dir}")
                print(f"# Command: {' '.join(sys.argv)}")
                print()
            except OSError as exc:
                print(f"Warning: could not open log file '{log_path}': {exc}",
                      file=sys.stderr)
                log_fh = None

        try:
            _run_cli(args)
        finally:
            sys.stdout = orig_out
            if log_fh:
                log_fh.close()
                print(f"Meta log written → '{log_path}'")

#!/usr/bin/env bash
# =============================================================================
# benchmark.sh — aimc_v5 vs industry-standard compressors
# Usage: ./benchmark.sh <input-directory> [output-log]
# =============================================================================
set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIMC="${AIMC:-${SCRIPT_DIR}/aim_C_v10}"       # override with AIMC=/path/to/aimc_v5

INPUT_DIR="${1:-}"
LOG_FILE="${2:-benchmark_$(date +%Y%m%d_%H%M%S).log}"
TMPDIR_BENCH="$(mktemp -d /tmp/bench_XXXXXX)"
trap 'rm -rf "$TMPDIR_BENCH"' EXIT

# ── Colours (disabled if not a tty) ──────────────────────────────────────────
if [ -t 1 ]; then
    RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
    BLU='\033[0;34m'; CYN='\033[0;36m'; BLD='\033[1m'; RST='\033[0m'
else
    RED=''; GRN=''; YLW=''; BLU=''; CYN=''; BLD=''; RST=''
fi

# ── Helpers ───────────────────────────────────────────────────────────────────
die()  { echo -e "${RED}ERROR: $*${RST}" >&2; exit 1; }
info() { echo -e "${BLU}  $*${RST}"; }
head_line() { printf "${BLD}%-26s %10s %10s %9s %12s${RST}\n" \
    "Compressor" "In (bytes)" "Out (bytes)" "Ratio %" "Time (s)"; }
sep_line()  { printf '%s\n' "$(printf '─%.0s' {1..72})"; }

log() { echo "$*" >> "$LOG_FILE"; }      # write to log only
tee_log() { echo "$*" | tee -a "$LOG_FILE"; }  # write to log + stdout

# ── Validate inputs ───────────────────────────────────────────────────────────
[ -z "$INPUT_DIR" ]    && die "No input directory given.\nUsage: $0 <dir> [log]"
[ -d "$INPUT_DIR" ]    || die "'$INPUT_DIR' is not a directory."
[ -x "$AIMC" ]         || die "aimc_v5 not found / not executable at: $AIMC\n  Set AIMC=/path/to/binary or place it next to this script."

# ── Detect available compressors ─────────────────────────────────────────────
# Format: "display_name|cmd_prefix|extension"
declare -a COMPRESSORS=(
    "aimc_v5|AIMC_SPECIAL|.aim3"
    "gzip -9|gzip -9 -c|.gz"
    "bzip2 -9|bzip2 -9 -c|.bz2"
    "xz -9|xz -9 -c|.xz"
    "zstd -3|zstd -3 -c|.zst3"
    "zstd -19|zstd -19 -c|.zst19"
    "lz4|lz4 -c|.lz4"
    "brotli -q11|brotli -q 11 -c|.br"
)

declare -a AVAILABLE=()
SKIP_WARN=()

for entry in "${COMPRESSORS[@]}"; do
    name="${entry%%|*}"; rest="${entry#*|}"; cmd="${rest%%|*}"
    if [ "$cmd" = "AIMC_SPECIAL" ]; then
        AVAILABLE+=("$entry")
    else
        bin="${cmd%% *}"
        if command -v "$bin" &>/dev/null; then
            AVAILABLE+=("$entry")
        else
            SKIP_WARN+=("$name ($bin not found)")
        fi
    fi
done

# ── Collect files ─────────────────────────────────────────────────────────────
mapfile -t FILES < <(find "$INPUT_DIR" -maxdepth 1 -type f | sort)
[ ${#FILES[@]} -eq 0 ] && die "No files found in '$INPUT_DIR'."

# ── Init log ──────────────────────────────────────────────────────────────────
: > "$LOG_FILE"   # truncate / create

log "# =============================================================="
log "# aimc_v5 Compression Benchmark"
log "# Run date    : $(date '+%Y-%m-%d %H:%M:%S')"
log "# Host        : $(uname -n)  ($(uname -m))"
log "# Kernel      : $(uname -r)"
log "# aimc binary : $AIMC"
log "# Input dir   : $(realpath "$INPUT_DIR")"
log "# Files       : ${#FILES[@]}"
log "# Log file    : $(realpath "$LOG_FILE")"
log "# =============================================================="
log ""

echo ""
echo -e "${CYN}${BLD}══════════════════════════════════════════════════════════════════════${RST}"
echo -e "${CYN}${BLD}  aimc_v5 Compression Benchmark${RST}"
echo -e "${CYN}  $(date '+%Y-%m-%d %H:%M:%S')  ·  ${#FILES[@]} file(s)  ·  log → $LOG_FILE${RST}"
echo -e "${CYN}${BLD}══════════════════════════════════════════════════════════════════════${RST}"

if [ ${#SKIP_WARN[@]} -gt 0 ]; then
    echo -e "${YLW}  Skipping (not installed): ${SKIP_WARN[*]}${RST}"
    log "# WARNING: skipped (not installed): ${SKIP_WARN[*]}"
    log ""
fi

# ── Accumulators for grand-total summary ─────────────────────────────────────
declare -A TOTAL_IN TOTAL_OUT TOTAL_TIME COUNT
for entry in "${AVAILABLE[@]}"; do
    name="${entry%%|*}"
    TOTAL_IN["$name"]=0; TOTAL_OUT["$name"]=0
    TOTAL_TIME["$name"]="0"; COUNT["$name"]=0
done

# ── bc helper (fallback to awk if bc unavailable) ────────────────────────────
calc() { awk "BEGIN { printf \"%.4f\", $1 }"; }

# ── Per-file loop ─────────────────────────────────────────────────────────────
FILE_IDX=0
for FILEPATH in "${FILES[@]}"; do
    FILE_IDX=$(( FILE_IDX + 1 ))
    FILENAME="$(basename "$FILEPATH")"
    ORIG_SIZE="$(wc -c < "$FILEPATH")"

    echo ""
    echo -e "${BLD}[$FILE_IDX/${#FILES[@]}] $FILENAME${RST}  (${ORIG_SIZE} bytes)"
    log ""
    log "# ── File $FILE_IDX / ${#FILES[@]}: $FILENAME ──────────────────────────────"
    log "[file]"
    log "name         = $FILENAME"
    log "path         = $(realpath "$FILEPATH")"
    log "original_bytes = $ORIG_SIZE"
    log ""

    head_line
    sep_line

    for entry in "${AVAILABLE[@]}"; do
        name="${entry%%|*}"; rest="${entry#*|}"; cmd="${rest%%|*}"; ext="${rest##*|}"
        OUT_FILE="${TMPDIR_BENCH}/${FILENAME}${ext}"
        AIM_LOG="${TMPDIR_BENCH}/aim_${FILE_IDX}.log"

        # ── Run compressor and measure wall time ────────────────────────────
        START_NS="$(date +%s%N)"

        if [ "$cmd" = "AIMC_SPECIAL" ]; then
            if "$AIMC" encode "$FILEPATH" "$OUT_FILE" --log "$AIM_LOG" \
                    >/dev/null 2>&1; then
                STATUS=ok
            else
                STATUS=fail
            fi
        else
            if $cmd < "$FILEPATH" > "$OUT_FILE" 2>/dev/null; then
                STATUS=ok
            else
                STATUS=fail
            fi
        fi

        END_NS="$(date +%s%N)"
        ELAPSED="$(calc "($END_NS - $START_NS) / 1000000000")"

        # ── Gather sizes ────────────────────────────────────────────────────
        if [ "$STATUS" = ok ] && [ -s "$OUT_FILE" ]; then
            COMP_SIZE="$(wc -c < "$OUT_FILE")"
            if [ "$ORIG_SIZE" -gt 0 ]; then
                RATIO="$(calc "$COMP_SIZE * 100 / $ORIG_SIZE")"
            else
                RATIO="N/A"
            fi
            # choose colour: green ≤100%, yellow ≤120%, red >120%
            RATIO_INT="${RATIO%%.*}"
            if   [ "$RATIO_INT" -le 100 ] 2>/dev/null; then C=$GRN
            elif [ "$RATIO_INT" -le 120 ] 2>/dev/null; then C=$YLW
            else C=$RED; fi
        else
            COMP_SIZE="ERROR"; RATIO="ERROR"; C=$RED
        fi

        # ── Print result row ────────────────────────────────────────────────
        printf "${BLD}%-26s${RST} %10s %10s ${C}%9s${RST} %12s\n" \
            "$name" "$ORIG_SIZE" "$COMP_SIZE" "${RATIO}%" "$ELAPSED"

        # ── Log structured entry ────────────────────────────────────────────
        log "[result.${name// /_}]"
        log "file         = $FILENAME"
        log "original_bytes = $ORIG_SIZE"
        log "compressed_bytes = $COMP_SIZE"
        log "ratio_pct    = ${RATIO}"
        log "time_s       = $ELAPSED"
        log "status       = $STATUS"

        # ── Append aimc internal log inline ─────────────────────────────────
        if [ "$cmd" = "AIMC_SPECIAL" ] && [ -f "$AIM_LOG" ]; then
            log ""
            log "# aimc_v5 internal log:"
            while IFS= read -r line; do log "  $line"; done < "$AIM_LOG"
        fi
        log ""

        # ── Accumulate totals ───────────────────────────────────────────────
        if [ "$STATUS" = ok ] && [ "$COMP_SIZE" != "ERROR" ]; then
            TOTAL_IN["$name"]=$(( ${TOTAL_IN[$name]} + ORIG_SIZE ))
            TOTAL_OUT["$name"]=$(( ${TOTAL_OUT[$name]} + COMP_SIZE ))
            TOTAL_TIME["$name"]="$(calc "${TOTAL_TIME[$name]} + $ELAPSED")"
            COUNT["$name"]=$(( ${COUNT[$name]} + 1 ))
        fi

        # Clean up temp compressed file
        rm -f "$OUT_FILE" "$AIM_LOG"
    done

    sep_line
done

# ── Grand summary ─────────────────────────────────────────────────────────────
echo ""
echo -e "${CYN}${BLD}══════════════════════════════  SUMMARY  ═══════════════════════════════${RST}"
echo -e "  All ${#FILES[@]} file(s) · totals across entire corpus"
echo ""

log ""
log "# =================================================================="
log "# SUMMARY — totals across all ${#FILES[@]} file(s)"
log "# =================================================================="
log ""

printf "${BLD}%-26s %10s %10s %9s %12s %8s${RST}\n" \
    "Compressor" "In (bytes)" "Out (bytes)" "Ratio %" "Total (s)" "Files"
sep_line

for entry in "${AVAILABLE[@]}"; do
    name="${entry%%|*}"
    n="${COUNT[$name]}"
    tin="${TOTAL_IN[$name]}"
    tout="${TOTAL_OUT[$name]}"
    ttime="${TOTAL_TIME[$name]}"

    if [ "$n" -gt 0 ] && [ "$tin" -gt 0 ]; then
        ratio="$(calc "$tout * 100 / $tin")"
        ri="${ratio%%.*}"
        if   [ "$ri" -le 100 ] 2>/dev/null; then C=$GRN
        elif [ "$ri" -le 120 ] 2>/dev/null; then C=$YLW
        else C=$RED; fi
    else
        ratio="N/A"; C=$RED
    fi

    printf "${BLD}%-26s${RST} %10s %10s ${C}%9s${RST} %12s %8s\n" \
        "$name" "$tin" "$tout" "${ratio}%" "$ttime" "${n}/${#FILES[@]}"

    log "[summary.${name// /_}]"
    log "total_in_bytes   = $tin"
    log "total_out_bytes  = $tout"
    log "overall_ratio_pct = $ratio"
    log "total_time_s     = $ttime"
    log "files_ok         = $n / ${#FILES[@]}"
    log ""
done

sep_line
echo ""
echo -e "${GRN}  Log written → $LOG_FILE${RST}"
echo ""

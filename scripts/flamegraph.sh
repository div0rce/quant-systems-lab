#!/usr/bin/env bash
# Generate a Linux perf flamegraph from the benchmark harness.
#
# Records call-graph samples with `perf record --call-graph dwarf`, folds them
# with scripts/flamegraph.py (a dependency-free stackcollapse + SVG renderer),
# and writes:
#   results/flamegraph.svg  -- the visual flamegraph (provenance embedded as a
#                              leading XML comment + a visible subtitle)
#   results/flamegraph.txt  -- provenance + classification + top folded stacks
#
# Defaults to software cpu-clock sampling so the artifact stays a portable
# hot-symbol *investigation* aid, not a latency/throughput claim. This is the
# missing-flamegraph follow-up tracked by issue #32 (the perf stat/record text
# workflow already exists; full hardware-PMU cache evidence stays in #90).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT_SVG="${QSL_FLAMEGRAPH_SVG:-results/flamegraph.svg}"
OUT_TXT="${QSL_FLAMEGRAPH_TXT:-results/flamegraph.txt}"
DATA="${QSL_FLAMEGRAPH_DATA:-build/perf/qsl-bench.flame.data}"
EVENT="${QSL_FLAMEGRAPH_EVENT:-cpu-clock}"
FREQ="${QSL_FLAMEGRAPH_FREQ:-4000}"
CALLGRAPH="${QSL_FLAMEGRAPH_CALLGRAPH:-dwarf}"
MIN_SAMPLES="${QSL_FLAMEGRAPH_MIN_SAMPLES:-200}"
TOP_STACKS="${QSL_FLAMEGRAPH_TOP_STACKS:-15}"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="flamegraph-benchmark"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/flamegraph.sh
    scripts/flamegraph.py
    scripts/qsl_common.sh
)

perf_version_line() {
    perf --version 2>&1 | head -1 || true
}

parse_sample_count_token() {
    awk -v raw="$1" '
        BEGIN {
            gsub(/,/, "", raw)
            suffix = substr(raw, length(raw), 1)
            mult = 1
            if (suffix == "K" || suffix == "k") { mult = 1000; raw = substr(raw, 1, length(raw) - 1) }
            else if (suffix == "M" || suffix == "m") { mult = 1000000; raw = substr(raw, 1, length(raw) - 1) }
            if (raw ~ /^[0-9]+([.][0-9]+)?$/) printf "%d\n", raw * mult
        }'
}

qsl_require_linux "scripts/flamegraph.sh" "perf"

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required to render the flamegraph." >&2
    exit 2
fi
if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make flamegraph)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT_SVG")" "$(dirname "$DATA")"

BENCH_OUT="$(mktemp)"
RECORD_BENCH_OUT="$(mktemp)"
RECORD_ERR="$(mktemp)"
SCRIPT_OUT="$(mktemp)"
SCRIPT_ERR="$(mktemp)"
FOLDED="$(mktemp)"
SVG_TMP="$(mktemp)"
TXT_TMP="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$RECORD_BENCH_OUT" "$RECORD_ERR" "$SCRIPT_OUT" "$SCRIPT_ERR" "$FOLDED" "$SVG_TMP" "$TXT_TMP"' EXIT

# Fail fast if the benchmark itself is broken (partial mode must not mask this).
BENCH_STATUS=0
"$BIN" >"$BENCH_OUT" 2>&1 || BENCH_STATUS=$?
if [[ "$BENCH_STATUS" -ne 0 ]]; then
    echo "error: benchmark command failed before perf record (status $BENCH_STATUS); partial mode cannot override this." >&2
    cat "$BENCH_OUT" >&2
    exit 4
fi

RECORD_STATUS=0
perf record --call-graph "$CALLGRAPH" -F "$FREQ" -g -e "$EVENT" -o "$DATA" -- "$BIN" \
    >"$RECORD_BENCH_OUT" 2>"$RECORD_ERR" || RECORD_STATUS=$?

SCRIPT_STATUS=0
if [[ "$RECORD_STATUS" -eq 0 ]]; then
    perf script -i "$DATA" >"$SCRIPT_OUT" 2>"$SCRIPT_ERR" || SCRIPT_STATUS=$?
fi

PERF_LIMITATION=no
if grep -Eiq 'No samples|failed to open|Permission denied|Operation not permitted|perf_event_open|not supported|Operation not supported|perf not found for kernel|linux-tools' \
    "$RECORD_ERR" "$SCRIPT_ERR"; then
    PERF_LIMITATION=yes
fi

SAMPLE_TOKEN="$(sed -nE 's/.*\(([0-9][0-9.,]*[KkMm]?) samples\).*/\1/p' "$RECORD_ERR" | head -1)"
SAMPLE_COUNT="$(parse_sample_count_token "$SAMPLE_TOKEN")"
[[ -z "$SAMPLE_COUNT" ]] && SAMPLE_COUNT=0

# Fold to collapsed stacks for the text summary and as an SVG precondition.
STACK_COUNT=0
if [[ "$SCRIPT_STATUS" -eq 0 && -s "$SCRIPT_OUT" ]]; then
    python3 scripts/flamegraph.py --collapse-only <"$SCRIPT_OUT" >"$FOLDED" 2>/dev/null || true
    STACK_COUNT="$(wc -l <"$FOLDED" | tr -d ' ')"
fi

INSUFFICIENT_SAMPLES=no
if [[ "$RECORD_STATUS" -eq 0 && "$SCRIPT_STATUS" -eq 0 && "$SAMPLE_COUNT" -lt "$MIN_SAMPLES" ]]; then
    INSUFFICIENT_SAMPLES=yes
fi

ARTIFACT_TYPE="flamegraph ($EVENT software sampling hot-symbol profile)"
if [[ "$EVENT" == "cycles" ]]; then
    ARTIFACT_TYPE="flamegraph (cycles hardware-PMU sampling hot-symbol profile)"
fi
if [[ "$RECORD_STATUS" -ne 0 || "$SCRIPT_STATUS" -ne 0 || "$STACK_COUNT" -eq 0 ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; no clean sample report)"
elif [[ "$INSUFFICIENT_SAMPLES" == "yes" ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; insufficient samples for hot-symbol conclusions)"
fi

PROVENANCE="$(qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT_SVG" "${PROVENANCE_INPUTS[@]}")"
HOST="$(uname -s) $(uname -m)"
DATE="$(qsl_utc_timestamp)"
SUBTITLE="$ARTIFACT_TYPE | $HOST | $EVENT @ ${FREQ}Hz | ${SAMPLE_COUNT} samples | ${STACK_COUNT} stacks | $DATE"

# Render the SVG (deterministic for a fixed folded input + fixed subtitle).
if [[ "$STACK_COUNT" -gt 0 ]]; then
    {
        echo '<?xml version="1.0" encoding="UTF-8" standalone="no"?>'
        # Keep the <!-- / --> delimiters on their own lines and squeeze any "--"
        # out of the interior: a double hyphen is illegal inside an XML comment.
        echo "<!--"
        {
            echo "QSL flamegraph provenance"
            echo "$PROVENANCE" | sed 's/^/     /'
            echo "     Command: make flamegraph"
            echo "     Artifact: $ARTIFACT_TYPE"
            echo "     Record: perf record [call-graph $CALLGRAPH | -F $FREQ | -g | -e $EVENT]"
            echo "     Samples: $SAMPLE_COUNT | Folded stacks: $STACK_COUNT"
            echo "     Caveat: software cpu-clock sampling shows on-CPU time by symbol; it is"
            echo "     not a latency or throughput measurement and is hardware/build dependent."
        } | sed 's/--/- -/g'
        echo "-->"
        # Drop the renderer's own XML declaration; we emitted ours above.
        python3 scripts/flamegraph.py \
            --title "QSL Matching-Engine Flame Graph (qsl-bench)" \
            --subtitle "$SUBTITLE" \
            --countname "$EVENT samples" \
            --from-collapsed <"$FOLDED" | tail -n +2
    } >"$SVG_TMP"
    qsl_publish_artifact "$SVG_TMP" "$OUT_SVG"
fi

# Text companion: provenance + classification + top folded stacks (human/queryable).
{
    echo "Command:       make flamegraph"
    echo "Artifact:      $ARTIFACT_TYPE"
    echo "Hardware:      $(uname -m)"
    echo "OS:            $(uname -s) $(uname -r)"
    echo "CPU:           $(qsl_cpu_model)"
    echo "Compiler:      $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Perf:          $(perf_version_line)"
    echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
    echo "Build type:    $(qsl_build_type "$BUILD_DIR")"
    echo "$PROVENANCE"
    echo "Benchmark binary: $BIN"
    echo "Dataset:       qsl-bench default synthetic benchmark suite"
    echo "Call graph:    $CALLGRAPH"
    echo "Record event:  $EVENT"
    echo "Sample freq:   $FREQ Hz"
    echo "Sample count:  $SAMPLE_COUNT"
    echo "Folded stacks: $STACK_COUNT"
    echo "Minimum samples for hot profile: $MIN_SAMPLES"
    echo "Insufficient samples: $INSUFFICIENT_SAMPLES"
    echo "Record status: $RECORD_STATUS"
    echo "Script status: $SCRIPT_STATUS"
    echo "Perf access limitation: $PERF_LIMITATION"
    echo "Flamegraph SVG: $(qsl_repo_relative_or_empty "$OUT_SVG")"
    echo "Perf data:     $DATA (generated, not intended for commit)"
    echo
    if [[ "$ARTIFACT_TYPE" == flamegraph* ]]; then
        echo "Caveat: this flamegraph is a software cpu-clock sampling profile for hot-symbol"
        echo "investigation. Frame width is proportional to on-CPU samples, not wall-clock"
        echo "latency or throughput, and is hardware/kernel/compiler/build dependent."
    else
        echo "Caveat: constrained/partial perf validation, not a hot-symbol flamegraph. Treat"
        echo "frame widths as unusable until sampling succeeds and Sample count meets the"
        echo "Minimum samples for hot profile."
    fi
    echo
    echo "Top $TOP_STACKS folded stacks (count  stack):"
    if [[ -s "$FOLDED" ]]; then
        # The final awk limits to $TOP_STACKS rows by reading all input (NR<=top)
        # rather than `head`, so `sort` is never sent SIGPIPE under `pipefail`.
        awk '{ n=$NF; $NF=""; sub(/[[:space:]]+$/,""); printf "%s\t%s\n", n, $0 }' "$FOLDED" |
            sort -t"$(printf '\t')" -k1,1nr |
            awk -F"$(printf '\t')" -v top="$TOP_STACKS" 'NR<=top { printf "%8d  %s\n", $1, $2 }'
    else
        echo "  (none)"
    fi
    echo
    echo "Benchmark output:"
    cat "$BENCH_OUT"
} >"$TXT_TMP"
qsl_publish_artifact "$TXT_TMP" "$OUT_TXT"
echo "wrote $OUT_TXT"
[[ "$STACK_COUNT" -gt 0 ]] && echo "wrote $OUT_SVG"

if [[ ("$RECORD_STATUS" -ne 0 || "$SCRIPT_STATUS" -ne 0) && "$PERF_LIMITATION" != "yes" ]]; then
    echo "error: perf record/script failed for a reason other than a perf access limitation." >&2
    exit 3
fi
if [[ "$STACK_COUNT" -eq 0 || "$INSUFFICIENT_SAMPLES" == "yes" ]]; then
    if [[ "${QSL_PERF_ALLOW_PARTIAL:-0}" != "1" ]]; then
        echo "error: flamegraph did not capture enough samples for a clean profile." >&2
        echo "       Re-run on Linux with perf sampling access, or set QSL_PERF_ALLOW_PARTIAL=1" >&2
        echo "       only when intentionally documenting a constrained environment." >&2
        exit 3
    fi
fi

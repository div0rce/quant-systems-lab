#!/usr/bin/env bash
# Generate a Linux perf flamegraph from the benchmark harness.
#
# Records call-graph samples with `perf record --call-graph fp` against the
# dedicated frame-pointer build (build/flamegraph, -fno-omit-frame-pointer -g)
# while qsl-bench runs its long-running `profile` workload, then folds them with
# scripts/flamegraph.py (a dependency-free stackcollapse + SVG renderer), and
# writes:
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

BIN="${QSL_BENCH_BIN:-build/flamegraph/qsl-bench}"
OUT_SVG="${QSL_FLAMEGRAPH_SVG:-results/flamegraph.svg}"
OUT_TXT="${QSL_FLAMEGRAPH_TXT:-results/flamegraph.txt}"
DATA="${QSL_FLAMEGRAPH_DATA:-build/perf/qsl-bench.flame.data}"
EVENT="${QSL_FLAMEGRAPH_EVENT:-cpu-clock}"
FREQ="${QSL_FLAMEGRAPH_FREQ:-4000}"
# Frame-pointer unwinding (the flamegraph preset keeps frame pointers) gives clean, fully-symbolized
# stacks; the prior dwarf default left [unknown] gaps because the Release bench build omits them.
CALLGRAPH="${QSL_FLAMEGRAPH_CALLGRAPH:-fp}"
# Seconds of warm steady-state order flow to sample. ~5s at -F 4000 yields tens of thousands of
# samples, versus the ~80ms (~329-sample) one-shot benchmark suite.
PROFILE_SECONDS="${QSL_FLAMEGRAPH_SECONDS:-5}"
BENCH_ARGS=(profile "$PROFILE_SECONDS")
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
COLLAPSE_ERR="$(mktemp)"
SVG_TMP="$(mktemp)"
TXT_TMP="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$RECORD_BENCH_OUT" "$RECORD_ERR" "$SCRIPT_OUT" "$SCRIPT_ERR" "$FOLDED" "$COLLAPSE_ERR" "$SVG_TMP" "$TXT_TMP"' EXIT

# Fail fast if the benchmark itself is broken (partial mode must not mask this). A short profile
# run validates the workload quickly without paying the full sampling duration.
BENCH_STATUS=0
"$BIN" profile 0.2 >"$BENCH_OUT" 2>&1 || BENCH_STATUS=$?
if [[ "$BENCH_STATUS" -ne 0 ]]; then
    echo "error: benchmark command failed before perf record (status $BENCH_STATUS); partial mode cannot override this." >&2
    cat "$BENCH_OUT" >&2
    exit 4
fi

RECORD_STATUS=0
perf record --call-graph "$CALLGRAPH" -F "$FREQ" -g -e "$EVENT" -o "$DATA" -- "$BIN" "${BENCH_ARGS[@]}" \
    >"$RECORD_BENCH_OUT" 2>"$RECORD_ERR" || RECORD_STATUS=$?

SCRIPT_STATUS=0
if [[ "$RECORD_STATUS" -eq 0 ]]; then
    perf script -i "$DATA" >"$SCRIPT_OUT" 2>"$SCRIPT_ERR" || SCRIPT_STATUS=$?
fi

PERF_LIMITATION=no
# `zero-sized data` is how `perf script` reports a no-sample capture; classify it
# as a perf limitation here exactly as scripts/perf_record.sh does, so the
# documented constrained-host (QSL_PERF_ALLOW_PARTIAL=1) path works instead of
# tripping the unexpected-failure exit.
if grep -Eiq 'zero-sized data|No samples|failed to open|Permission denied|Operation not permitted|perf_event_open|not supported|Operation not supported|perf not found for kernel|linux-tools' \
    "$RECORD_ERR" "$SCRIPT_ERR"; then
    PERF_LIMITATION=yes
fi

# perf record prints its sample summary as "(N samples)" or, on some versions,
# "(~N samples)", and that count is only its own estimate. Accept the optional
# `~` so the token is not dropped, but keep this value informational; the sample
# gate below uses the authoritative folded total, not this estimate.
SAMPLE_TOKEN="$(sed -nE 's/.*\(~?([0-9][0-9.,]*[KkMm]?) samples\).*/\1/p' "$RECORD_ERR" | head -1)"
PERF_EST_SAMPLES="$(parse_sample_count_token "$SAMPLE_TOKEN")"
[[ -z "$PERF_EST_SAMPLES" ]] && PERF_EST_SAMPLES=0

# Fold to collapsed stacks for the text summary and as an SVG precondition. A
# nonzero COLLAPSE_STATUS means the renderer/parser itself failed (a generator
# regression), which is handled as an unexpected failure below, never masked as
# a perf sampling limitation. FOLDED_SAMPLES is the real sample total carried by
# the folded stacks (sum of trailing counts), the authoritative gate input.
STACK_COUNT=0
FOLDED_SAMPLES=0
COLLAPSE_STATUS=0
if [[ "$SCRIPT_STATUS" -eq 0 && -s "$SCRIPT_OUT" ]]; then
    python3 scripts/flamegraph.py --collapse-only <"$SCRIPT_OUT" >"$FOLDED" 2>"$COLLAPSE_ERR" ||
        COLLAPSE_STATUS=$?
    STACK_COUNT="$(wc -l <"$FOLDED" | tr -d ' ')"
    FOLDED_SAMPLES="$(awk '{ s += $NF } END { printf "%d\n", s + 0 }' "$FOLDED")"
fi

INSUFFICIENT_SAMPLES=no
if [[ "$RECORD_STATUS" -eq 0 && "$SCRIPT_STATUS" -eq 0 && "$COLLAPSE_STATUS" -eq 0 &&
    "$FOLDED_SAMPLES" -lt "$MIN_SAMPLES" ]]; then
    INSUFFICIENT_SAMPLES=yes
fi

# Describe the sampling source once so every label/caveat (artifact type, SVG
# comment, text companion) stays consistent: software timers vs a hardware PMU
# event. cpu-clock/task-clock are software; cycles/instructions/etc. are PMU.
case "$EVENT" in
cpu-clock | task-clock) SAMPLE_KIND="software $EVENT sampling" ;;
*) SAMPLE_KIND="$EVENT hardware-PMU sampling" ;;
esac
ARTIFACT_TYPE="flamegraph ($SAMPLE_KIND hot-symbol profile)"
if [[ "$RECORD_STATUS" -ne 0 || "$SCRIPT_STATUS" -ne 0 || "$STACK_COUNT" -eq 0 ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; no clean sample report)"
elif [[ "$INSUFFICIENT_SAMPLES" == "yes" ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; insufficient samples for hot-symbol conclusions)"
fi

PROVENANCE="$(qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT_SVG" "${PROVENANCE_INPUTS[@]}")"
HOST="$(uname -s) $(uname -m)"
DATE="$(qsl_utc_timestamp)"
SUBTITLE="$ARTIFACT_TYPE | $HOST | $EVENT @ ${FREQ}Hz | ${FOLDED_SAMPLES} samples | ${STACK_COUNT} stacks | $DATE"

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
            echo "     Samples (folded): $FOLDED_SAMPLES | perf record estimate: $PERF_EST_SAMPLES | Folded stacks: $STACK_COUNT"
            echo "     Caveat: $SAMPLE_KIND shows on-CPU time by symbol; it is not a latency"
            echo "     or throughput measurement and is hardware/build dependent."
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
else
    # No clean folded stacks. Remove any prior SVG so a constrained rerun cannot
    # leave a previous host's flamegraph beside a .txt that says there is no
    # sample report, which could be committed as if the two still matched.
    rm -f "$OUT_SVG"
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
    echo "Dataset:       qsl-bench profile workload (warm bounded order flow, ${PROFILE_SECONDS}s)"
    echo "Call graph:    $CALLGRAPH"
    echo "Record event:  $EVENT"
    echo "Sample freq:   $FREQ Hz"
    echo "Sample count (folded total):      $FOLDED_SAMPLES"
    echo "Sample count (perf record est.):  $PERF_EST_SAMPLES"
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
        echo "Caveat: this flamegraph is a $SAMPLE_KIND profile for hot-symbol"
        echo "investigation. Frame width is proportional to on-CPU samples, not wall-clock"
        echo "latency or throughput, and is hardware/kernel/compiler/build dependent."
    else
        echo "Caveat: constrained/partial perf validation, not a hot-symbol flamegraph. Treat"
        echo "frame widths as unusable until sampling succeeds and the folded sample total"
        echo "meets the Minimum samples for hot profile."
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
    # Prefer the actually-sampled run's summary; fall back to the fail-fast pre-check on a
    # partial/failed record so the section is never empty.
    if [[ -s "$RECORD_BENCH_OUT" ]]; then
        cat "$RECORD_BENCH_OUT"
    else
        cat "$BENCH_OUT"
    fi
} >"$TXT_TMP"
qsl_publish_artifact "$TXT_TMP" "$OUT_TXT"
echo "wrote $OUT_TXT"
[[ "$STACK_COUNT" -gt 0 ]] && echo "wrote $OUT_SVG"

# A renderer/parser failure (perf script succeeded but flamegraph.py errored) is
# a generator bug, not a perf sampling limitation, fail hard so partial mode
# cannot publish a Python/parser regression as a constrained-environment artifact.
if [[ "$SCRIPT_STATUS" -eq 0 && "$COLLAPSE_STATUS" -ne 0 ]]; then
    echo "error: flamegraph.py --collapse-only failed (status $COLLAPSE_STATUS); this is a renderer/parser failure, not a perf limitation, and partial mode cannot mask it." >&2
    cat "$COLLAPSE_ERR" >&2
    exit 4
fi
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

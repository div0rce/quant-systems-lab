#!/usr/bin/env bash
# Run Linux perf record/report against the benchmark harness and write a text report.
# Defaults to software cpu-clock sampling so constrained Linux environments can still identify
# hot symbols even when hardware PMU counters are unavailable.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_PERF_REPORT_OUT:-results/perf_report_linux.txt}"
DATA="${QSL_PERF_DATA:-build/perf/qsl-bench.perf.data}"
EVENT="${QSL_PERF_RECORD_EVENT:-cpu-clock}"
FREQ="${QSL_PERF_RECORD_FREQ:-2000}"
LIMIT="${QSL_PERF_REPORT_PERCENT_LIMIT:-1}"
MIN_SAMPLES="${QSL_PERF_MIN_SAMPLES:-100}"

parse_sample_count_token() {
    local token="$1"
    awk -v raw="$token" '
        BEGIN {
            gsub(/,/, "", raw)
            suffix = substr(raw, length(raw), 1)
            mult = 1
            if (suffix == "K" || suffix == "k") {
                mult = 1000
                raw = substr(raw, 1, length(raw) - 1)
            } else if (suffix == "M" || suffix == "m") {
                mult = 1000000
                raw = substr(raw, 1, length(raw) - 1)
            }
            if (raw ~ /^[0-9]+([.][0-9]+)?$/) {
                printf "%d\n", raw * mult
            }
        }'
}

qsl_require_linux "scripts/perf_record.sh" "perf"

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make perf-record)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")" "$(dirname "$DATA")"

DIRTY="$(qsl_dirty_tree_status results/perf_stat_linux.txt results/perf_report_linux.txt "$OUT" "$DATA")"

BENCH_OUT="$(mktemp)"
RECORD_BENCH_OUT="$(mktemp)"
RECORD_ERR="$(mktemp)"
REPORT_OUT="$(mktemp)"
REPORT_ERR="$(mktemp)"
TMP_OUT="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$RECORD_BENCH_OUT" "$RECORD_ERR" "$REPORT_OUT" "$REPORT_ERR" "$TMP_OUT"' EXIT

BENCH_STATUS=0
"$BIN" >"$BENCH_OUT" 2>&1 || BENCH_STATUS=$?

if [[ "$BENCH_STATUS" -ne 0 ]]; then
    {
        echo "Command:       make perf-record"
        echo "Artifact:      failed benchmark run (not perf evidence)"
        echo "Hardware:      $(uname -m)"
        echo "OS:            $(uname -s) $(uname -r)"
        echo "CPU:           $(qsl_cpu_model)"
        echo "Compiler:      $(qsl_compiler_version)"
        echo "Perf:          $(perf --version)"
        echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
        echo "Build type:    Release"
        echo "Git commit:    $(qsl_git_commit_short)"
        echo "Dirty tree:    $DIRTY"
        echo "Benchmark binary: $BIN"
        echo "Benchmark status: $BENCH_STATUS"
        echo "Dataset:       qsl-bench default synthetic benchmark suite"
        echo "Record event:  $EVENT"
        echo "Sample freq:   $FREQ Hz"
        echo "Minimum samples for hot profile: $MIN_SAMPLES"
        echo "Date:          $(qsl_utc_timestamp)"
        echo
        echo "Benchmark output:"
        cat "$BENCH_OUT"
    } >"$TMP_OUT"
    mv "$TMP_OUT" "$OUT"
    echo "wrote $OUT"
    cat "$OUT"
    echo "error: benchmark command failed before perf record; partial mode cannot override this." >&2
    exit 4
fi

RECORD_STATUS=0
perf record -F "$FREQ" -g -e "$EVENT" -o "$DATA" -- "$BIN" >"$RECORD_BENCH_OUT" 2>"$RECORD_ERR" ||
    RECORD_STATUS=$?

REPORT_STATUS=0
if [[ "$RECORD_STATUS" -eq 0 ]]; then
    perf report --stdio --no-children --sort symbol,dso -i "$DATA" --percent-limit "$LIMIT" \
        >"$REPORT_OUT" 2>"$REPORT_ERR" || REPORT_STATUS=$?
fi

NO_SAMPLES=no
if grep -Eiq 'zero-sized data|No samples|failed to open|Permission denied|Operation not permitted' "$RECORD_ERR" "$REPORT_OUT" "$REPORT_ERR"; then
    NO_SAMPLES=yes
fi

PERF_LIMITATION=no
if grep -Eiq 'zero-sized data|No samples|failed to open|Permission denied|Operation not permitted|perf_event_open|not supported|Operation not supported' "$RECORD_ERR" "$REPORT_OUT" "$REPORT_ERR"; then
    PERF_LIMITATION=yes
fi

SAMPLE_TOKEN="$(sed -nE 's/^# Samples:[[:space:]]*([^[:space:]]+).*/\1/p' "$REPORT_OUT" | head -1)"
SAMPLE_COUNT="$(parse_sample_count_token "$SAMPLE_TOKEN")"
if [[ -z "$SAMPLE_COUNT" ]]; then
    SAMPLE_TOKEN="$(sed -nE 's/.*\(([0-9][0-9.,]*[KkMm]?) samples\).*/\1/p' "$RECORD_ERR" |
        head -1)"
    SAMPLE_COUNT="$(parse_sample_count_token "$SAMPLE_TOKEN")"
fi
if [[ -z "$SAMPLE_COUNT" ]]; then
    SAMPLE_COUNT=0
fi

INSUFFICIENT_SAMPLES=no
if [[ "$RECORD_STATUS" -eq 0 && "$REPORT_STATUS" -eq 0 && "$NO_SAMPLES" == "no" &&
    "$SAMPLE_COUNT" -lt "$MIN_SAMPLES" ]]; then
    INSUFFICIENT_SAMPLES=yes
fi

ARTIFACT_TYPE="software sampling hot-symbol profile"
if [[ "$RECORD_STATUS" -ne 0 || "$REPORT_STATUS" -ne 0 || "$NO_SAMPLES" == "yes" ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; no clean sample report)"
elif [[ "$INSUFFICIENT_SAMPLES" == "yes" ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; insufficient samples for hot-symbol conclusions)"
fi

{
    echo "Command:       make perf-record"
    echo "Artifact:      $ARTIFACT_TYPE"
    echo "Hardware:      $(uname -m)"
    echo "OS:            $(uname -s) $(uname -r)"
    echo "CPU:           $(qsl_cpu_model)"
    echo "Compiler:      $(qsl_compiler_version)"
    echo "Perf:          $(perf --version)"
    echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
    echo "Build type:    Release"
    echo "Git commit:    $(qsl_git_commit_short)"
    echo "Dirty tree:    $DIRTY"
    echo "Benchmark binary: $BIN"
    echo "Benchmark status: $BENCH_STATUS"
    echo "Dataset:       qsl-bench default synthetic benchmark suite"
    echo "Record event:  $EVENT"
    echo "Sample freq:   $FREQ Hz"
    echo "Sample count:  $SAMPLE_COUNT"
    echo "Minimum samples for hot profile: $MIN_SAMPLES"
    echo "Insufficient samples: $INSUFFICIENT_SAMPLES"
    echo "Report limit:  $LIMIT%"
    echo "Record status: $RECORD_STATUS"
    echo "Report status: $REPORT_STATUS"
    echo "No samples:    $NO_SAMPLES"
    echo "Perf access limitation: $PERF_LIMITATION"
    echo "Perf data:     $DATA (generated, not intended for commit)"
    echo "Date:          $(qsl_utc_timestamp)"
    echo
    if [[ "$ARTIFACT_TYPE" == "software sampling hot-symbol profile" ]]; then
        echo "Caveat: perf report is hardware/kernel/compiler/build dependent. The default"
        echo "cpu-clock event is a software sampling profile for hot-symbol investigation,"
        echo "not a latency or throughput measurement."
    else
        echo "Caveat: this is constrained/partial perf-record validation, not hot-symbol"
        echo "evidence. Treat symbol ordering as unusable until sampling succeeds and"
        echo "Sample count meets Minimum samples for hot profile."
    fi
    echo
    echo "Benchmark output:"
    cat "$BENCH_OUT"
    echo
    echo "Benchmark output under perf:"
    cat "$RECORD_BENCH_OUT"
    echo
    echo "perf record stderr:"
    cat "$RECORD_ERR"
    echo
    echo "perf report stderr:"
    cat "$REPORT_ERR"
    echo
    echo "perf report output:"
    cat "$REPORT_OUT"
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

if [[ ("$RECORD_STATUS" -ne 0 || "$REPORT_STATUS" -ne 0) && "$PERF_LIMITATION" != "yes" ]]; then
    echo "error: perf record/report failed for a reason other than a perf access/sample limitation." >&2
    echo "       Partial mode cannot override benchmark or unexpected perf failures." >&2
    exit 3
fi

if [[ "$NO_SAMPLES" == "yes" || "$INSUFFICIENT_SAMPLES" == "yes" ||
    "$RECORD_STATUS" -ne 0 || "$REPORT_STATUS" -ne 0 ]]; then
    if [[ "${QSL_PERF_ALLOW_PARTIAL:-0}" != "1" ]]; then
        echo "error: perf record/report did not produce a clean sample report." >&2
        echo "       Re-run on Linux with perf sampling access, or set QSL_PERF_ALLOW_PARTIAL=1" >&2
        echo "       only when intentionally documenting a constrained environment." >&2
        exit 3
    fi
fi

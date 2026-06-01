#!/usr/bin/env bash
# Run Linux perf record/report against the benchmark harness and write a text report.
# Defaults to software cpu-clock sampling so constrained Linux environments can still identify
# hot symbols even when hardware PMU counters are unavailable.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_PERF_REPORT_OUT:-results/perf_report_linux.txt}"
DATA="${QSL_PERF_DATA:-build/perf/qsl-bench.perf.data}"
EVENT="${QSL_PERF_RECORD_EVENT:-cpu-clock}"
FREQ="${QSL_PERF_RECORD_FREQ:-99}"
LIMIT="${QSL_PERF_REPORT_PERCENT_LIMIT:-1}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: scripts/perf_record.sh requires Linux perf; current OS is $(uname -s)." >&2
    exit 2
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make perf-record)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")" "$(dirname "$DATA")"

DIRTY=no
if [[ -n "$(git status --porcelain --untracked-files=all -- . \
    ":(exclude)results/perf_stat_linux.txt" \
    ":(exclude)results/perf_report_linux.txt" \
    ":(exclude)$OUT" \
    ":(exclude)$DATA")" ]]; then
    DIRTY=yes
fi

BENCH_OUT="$(mktemp)"
RECORD_ERR="$(mktemp)"
REPORT_OUT="$(mktemp)"
REPORT_ERR="$(mktemp)"
TMP_OUT="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$RECORD_ERR" "$REPORT_OUT" "$REPORT_ERR" "$TMP_OUT"' EXIT

RECORD_STATUS=0
perf record -F "$FREQ" -g -e "$EVENT" -o "$DATA" -- "$BIN" >"$BENCH_OUT" 2>"$RECORD_ERR" ||
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

{
    echo "Command:       make perf-record"
    echo "Hardware:      $(uname -m)"
    echo "OS:            $(uname -s) $(uname -r)"
    echo "CPU:           $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
    echo "Compiler:      $(c++ --version | head -1)"
    echo "Perf:          $(perf --version)"
    echo "Build type:    Release"
    echo "Git commit:    $(git rev-parse --short HEAD)"
    echo "Dirty tree:    $DIRTY"
    echo "Dataset:       qsl-bench default synthetic benchmark suite"
    echo "Record event:  $EVENT"
    echo "Sample freq:   $FREQ Hz"
    echo "Report limit:  $LIMIT%"
    echo "Record status: $RECORD_STATUS"
    echo "Report status: $REPORT_STATUS"
    echo "No samples:    $NO_SAMPLES"
    echo "Perf data:     $DATA (generated, not intended for commit)"
    echo "Date:          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Caveat: perf report is hardware/kernel/compiler/build dependent. The default"
    echo "cpu-clock event is a software sampling profile for hot-symbol investigation,"
    echo "not a latency or throughput measurement."
    echo
    echo "Benchmark output:"
    cat "$BENCH_OUT"
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

if [[ "$RECORD_STATUS" -ne 0 || "$REPORT_STATUS" -ne 0 || "$NO_SAMPLES" == "yes" ]]; then
    if [[ "${QSL_PERF_ALLOW_PARTIAL:-0}" != "1" ]]; then
        echo "error: perf record/report did not produce a clean sample report." >&2
        echo "       Re-run on Linux with perf sampling access, or set QSL_PERF_ALLOW_PARTIAL=1" >&2
        echo "       only when intentionally documenting a constrained environment." >&2
        exit 3
    fi
fi

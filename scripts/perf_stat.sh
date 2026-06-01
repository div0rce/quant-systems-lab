#!/usr/bin/env bash
# Run Linux perf stat against the benchmark harness and write metadata-rich output.
# Hardware counters require a Linux kernel/host that exposes PMU events to the process.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_PERF_STAT_OUT:-results/perf_stat_linux.txt}"
EVENTS="${QSL_PERF_STAT_EVENTS:-cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,page-faults}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: scripts/perf_stat.sh requires Linux perf; current OS is $(uname -s)." >&2
    exit 2
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make perf-stat)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

DIRTY=no
if [[ -n "$(git status --porcelain --untracked-files=all -- . ":(exclude)$OUT")" ]]; then
    DIRTY=yes
fi

BENCH_OUT="$(mktemp)"
PERF_OUT="$(mktemp)"
TMP_OUT="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$PERF_OUT" "$TMP_OUT"' EXIT

STATUS=0
perf stat -e "$EVENTS" -- "$BIN" >"$BENCH_OUT" 2>"$PERF_OUT" || STATUS=$?

UNSUPPORTED=no
if grep -Eiq '<not supported>|not supported|No permission|not counted|Operation not permitted|Permission denied' "$PERF_OUT"; then
    UNSUPPORTED=yes
fi

{
    echo "Command:     make perf-stat"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Perf:        $(perf --version)"
    echo "Build type:  Release"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Dataset:     qsl-bench default synthetic benchmark suite"
    echo "Events:      $EVENTS"
    echo "Perf status: $STATUS"
    echo "Unsupported counters detected: $UNSUPPORTED"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Caveat: Linux perf counters are hardware/kernel/permission dependent."
    echo "This is profiling evidence for investigation, not a production-latency claim."
    echo
    echo "Benchmark output:"
    cat "$BENCH_OUT"
    echo
    echo "perf stat output:"
    cat "$PERF_OUT"
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

if [[ "$STATUS" -ne 0 || "$UNSUPPORTED" == "yes" ]]; then
    if [[ "${QSL_PERF_ALLOW_PARTIAL:-0}" != "1" ]]; then
        echo "error: perf stat did not capture the required hardware counters cleanly." >&2
        echo "       Re-run on a Linux host with PMU access, or set QSL_PERF_ALLOW_PARTIAL=1" >&2
        echo "       only when intentionally documenting a constrained environment." >&2
        exit 3
    fi
fi

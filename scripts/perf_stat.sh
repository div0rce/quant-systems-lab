#!/usr/bin/env bash
# Run Linux perf stat against the benchmark harness and write metadata-rich output.
# Hardware counters require a Linux kernel/host that exposes PMU events to the process.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_PERF_STAT_OUT:-results/perf_stat_linux.txt}"
EVENTS="${QSL_PERF_STAT_EVENTS:-cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,page-faults}"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="perf-stat-benchmark"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/perf_stat.sh
    scripts/qsl_common.sh
)

perf_version_line() {
    perf --version 2>&1 | head -1 || true
}

qsl_require_linux "scripts/perf_stat.sh" "perf"

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make perf-stat)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

BENCH_OUT="$(mktemp)"
PERF_BENCH_OUT="$(mktemp)"
PERF_OUT="$(mktemp)"
TMP_OUT="$(mktemp)"
trap 'rm -f "$BENCH_OUT" "$PERF_BENCH_OUT" "$PERF_OUT" "$TMP_OUT"' EXIT

BENCH_STATUS=0
"$BIN" >"$BENCH_OUT" 2>&1 || BENCH_STATUS=$?

if [[ "$BENCH_STATUS" -ne 0 ]]; then
    {
        echo "Command:     make perf-stat"
        echo "Artifact:    failed benchmark run (not perf evidence)"
        echo "Hardware:    $(uname -m)"
        echo "OS:          $(uname -s) $(uname -r)"
        echo "CPU:         $(qsl_cpu_model)"
        echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
        echo "Perf:        $(perf_version_line)"
        echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
        echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
        qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
        echo "Benchmark binary: $BIN"
        echo "Benchmark status: $BENCH_STATUS"
        echo "Dataset:     qsl-bench default synthetic benchmark suite"
        echo "Events:      $EVENTS"
        echo
        echo "Benchmark output:"
        cat "$BENCH_OUT"
    } >"$TMP_OUT"
    qsl_publish_artifact "$TMP_OUT" "$OUT"
    echo "wrote $OUT"
    cat "$OUT"
    echo "error: benchmark command failed before perf stat; partial mode cannot override this." >&2
    exit 4
fi

PERF_STATUS=0
perf stat -e "$EVENTS" -- "$BIN" >"$PERF_BENCH_OUT" 2>"$PERF_OUT" || PERF_STATUS=$?

UNSUPPORTED=no
if grep -Eiq '<not supported>|not supported|No permission|not counted|Operation not permitted|Permission denied|perf not found for kernel|linux-tools' "$PERF_OUT"; then
    UNSUPPORTED=yes
fi

ARTIFACT_TYPE="hardware PMU evidence"
HARDWARE_COUNTERS_SUPPORTED=yes
if [[ "$UNSUPPORTED" == "yes" ]]; then
    ARTIFACT_TYPE="constrained-environment validation (partial; not full hardware PMU evidence)"
    HARDWARE_COUNTERS_SUPPORTED=no
elif [[ "$PERF_STATUS" -ne 0 ]]; then
    ARTIFACT_TYPE="failed perf stat run (not accepted evidence)"
fi

{
    echo "Command:     make perf-stat"
    echo "Artifact:    $ARTIFACT_TYPE"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(qsl_cpu_model)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Perf:        $(perf_version_line)"
    echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Benchmark binary: $BIN"
    echo "Benchmark status: $BENCH_STATUS"
    echo "Dataset:     qsl-bench default synthetic benchmark suite"
    echo "Events:      $EVENTS"
    echo "Perf status: $PERF_STATUS"
    echo "Unsupported counters detected: $UNSUPPORTED"
    echo "Hardware counters supported: $HARDWARE_COUNTERS_SUPPORTED"
    echo
    echo "Caveat: Linux perf counters are hardware/kernel/permission dependent."
    echo "Partial artifacts document environment behavior only; they are not full PMU evidence."
    echo "This is profiling evidence for investigation, not a production-latency claim."
    echo
    echo "Benchmark output:"
    cat "$BENCH_OUT"
    echo
    echo "Benchmark output under perf:"
    cat "$PERF_BENCH_OUT"
    echo
    echo "perf stat output:"
    cat "$PERF_OUT"
} >"$TMP_OUT"

qsl_publish_artifact "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

if [[ "$PERF_STATUS" -ne 0 && "$UNSUPPORTED" != "yes" ]]; then
    echo "error: perf stat failed for a reason other than unsupported/permission-limited counters." >&2
    echo "       Partial mode cannot override benchmark or unexpected perf failures." >&2
    exit 3
fi

if [[ "$UNSUPPORTED" == "yes" ]]; then
    if [[ "${QSL_PERF_ALLOW_PARTIAL:-0}" != "1" ]]; then
        echo "error: perf stat did not capture the required hardware counters cleanly." >&2
        echo "       Re-run on a Linux host with PMU access, or set QSL_PERF_ALLOW_PARTIAL=1" >&2
        echo "       only when intentionally documenting a constrained environment." >&2
        exit 3
    fi
fi

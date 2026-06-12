#!/usr/bin/env bash
# Run the M46 recovery benchmark and write a metadata-rich results file.
# All numbers are produced by the committed benchmark harness; none are hand-written.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_RECOVERY_BENCH_OUT:-results/recovery_benchmarks.txt}"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="recovery-benchmark"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/run_recovery_benchmarks.sh
    scripts/qsl_common.sh
)

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-recovery)." >&2
    exit 1
fi

{
    echo "Command:     make bench-recovery"
    echo "Hardware:    $(uname -m) ($(qsl_cpu_model))"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:     deterministic generated flows (seed 42, 4 symbols, 5k/20k/80k commands)"
    echo "             plus synthetic non-crossing resting books (1k/10k/50k resting orders)"
    echo "Scenario:    full-replay restart (recover_log_file + replay) vs in-memory book rebuild"
    echo "Warmup:      one untimed run per phase before timing; averages over the listed reps"
    echo "Units:       ms/run plus ns per record/command/order"
    echo
    echo "Recovery objective measured: time to rebuild engine state after a process crash, on"
    echo "this host, from a clean event log of the given length (restart cost; RTO-style)."
    echo "How much acknowledged data can be lost before restart (RPO-style) is governed by the"
    echo "M45 durability modes and is exercised by make crash-recovery, not measured here."
    echo
    echo "Caveat: synthetic single-process benchmark (release build, local filesystem, warm page"
    echo "cache, no concurrent load). Full-replay restart cost grows with log length. The"
    echo "snapshot-restoration prototype is benchmark-only and in-memory - no snapshot"
    echo "persistence exists in this repo - so its numbers are a lower bound that excludes"
    echo "serialization, disk I/O, and tail replay past the snapshot point. Hardware-, compiler-,"
    echo "and build-dependent; not a production recovery-time claim."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN" recovery
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

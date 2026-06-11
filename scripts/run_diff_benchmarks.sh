#!/usr/bin/env bash
# Run the differential-testing harness benchmarks (release build) and write a results file with
# full metadata. All numbers are produced here; none are hand-written. Hardware-dependent. Kept
# separate from results/latest.txt so the core engine/protocol numbers are not disturbed.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="results/differential.txt"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="differential-benchmark-suite"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/run_diff_benchmarks.sh
    scripts/qsl_common.sh
)

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-diff)." >&2
    exit 1
fi

{
    echo "Command:     make bench-diff"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:     property command streams (generate_property_flow, 3 symbols, 120 orders)"
    echo "Warmup:      iters/10 (or 1 throughput pass) per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec; throughput = ns/item + items/sec"
    echo
    echo "Caveat: single-process synthetic microbenchmarks (hot cache, no network/disk). These"
    echo "measure the differential-testing harness (generation, gateway replay, shrinking), not"
    echo "production throughput; hardware/compiler/build dependent."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN" diff
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

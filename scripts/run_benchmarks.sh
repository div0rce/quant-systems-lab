#!/usr/bin/env bash
# Run the benchmark harness (release build) and write a results file with full metadata.
# All numbers are produced here; none are hand-written. Results are hardware-dependent.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/release/qsl-bench}"
OUT="results/latest.txt"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="core-benchmark-suite"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/run_benchmarks.sh
    scripts/qsl_common.sh
)

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench)." >&2
    exit 1
fi

{
    echo "Command:     make bench"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:     synthetic order flow (replay::generate_flow, seed 42, 4 symbols)"
    echo "Warmup:      iters/10 (or 1 throughput pass) per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec; throughput = ns/item + items/sec"
    echo
    echo "Caveat: single-process synthetic microbenchmarks (hot cache, no network, no disk,"
    echo "no kernel/IO path, stock allocator). NOT production exchange throughput or"
    echo "end-to-end latency; hardware/compiler/build dependent."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN"
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

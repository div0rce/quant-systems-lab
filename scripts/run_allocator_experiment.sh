#!/usr/bin/env bash
# Run the M28 allocator experiment and write a metadata-rich results file.
# All numbers are produced by the committed benchmark harness; none are hand-written.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="results/allocator_experiment.txt"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="allocator-experiment"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/run_allocator_experiment.sh
    scripts/qsl_common.sh
)

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-allocator)." >&2
    exit 1
fi

{
    echo "Command:     make bench-allocator"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:     engine::Order allocation microbenchmark (new/delete vs fixed pool)"
    echo "Warmup:      iters/10 per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec"
    echo
    echo "Caveat: isolated allocation microbenchmark (hot cache, single process, no network/disk)."
    echo "This measures allocator mechanics for order-like objects, not end-to-end engine latency;"
    echo "hardware/compiler/build dependent. A negative or tiny delta is acceptable."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN" pool
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

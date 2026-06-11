#!/usr/bin/env bash
# Run the M32 pool-backed order-book storage experiment and write a metadata-rich results file.
# All numbers are produced by the committed benchmark harness; none are hand-written.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_STORAGE_BENCH_OUT:-results/pool_backed_storage.txt}"
BUILD_DIR="$(dirname "$BIN")"
PROVENANCE_SCOPE="pool-backed-storage-benchmark"
PROVENANCE_INPUTS=(
    Makefile
    CMakeLists.txt
    CMakePresets.json
    cmake
    include
    src
    apps/qsl-bench
    benchmarks
    scripts/run_storage_benchmarks.sh
    scripts/qsl_common.sh
)

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-storage)." >&2
    exit 1
fi

{
    echo "Command:     make bench-storage"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(qsl_build_compiler_version "$BUILD_DIR")"
    echo "Build type:  $(qsl_build_type "$BUILD_DIR")"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:     deterministic generated engine flow (seed 42, 4 symbols, 5000 commands)"
    echo "Scenario:    baseline OrderBook storage vs PMR pooled node allocation vs intrusive OrderPool nodes"
    echo "Warmup:      one full engine replay per storage mode before timing"
    echo "Units:       throughput = ns/command + commands/sec"
    echo
    echo "Caveat: engine-level synthetic benchmark (single process, release build, no network/disk)."
    echo "M32 uses std::pmr::unsynchronized_pool_resource for list/map/unordered_map node allocation."
    echo "The intrusive mode uses M28 OrderPool<Capacity> for resting orders plus custom FIFO nodes;"
    echo "price and index maps remain standard containers. Hardware/compiler/build dependent."
    echo "A neutral or negative result is acceptable and should not be reported as a speedup."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN" storage
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

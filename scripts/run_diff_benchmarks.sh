#!/usr/bin/env bash
# Run the differential-testing harness benchmarks (release build) and write a results file with
# full metadata. All numbers are produced here; none are hand-written. Hardware-dependent. Kept
# separate from results/latest.txt so the core engine/protocol numbers are not disturbed.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="results/differential.txt"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-diff)." >&2
    exit 1
fi

DIRTY=no
if [[ -n "$(git status --porcelain)" ]]; then DIRTY=yes; fi

{
    echo "Command:     make bench-diff"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Build type:  Release"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Dataset:     property command streams (generate_property_flow, 3 symbols, 120 orders)"
    echo "Warmup:      iters/10 (or 1 throughput pass) per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec; throughput = ns/item + items/sec"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
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

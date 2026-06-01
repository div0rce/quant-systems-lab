#!/usr/bin/env bash
# Run the M28 allocator experiment and write a metadata-rich results file.
# All numbers are produced by the committed benchmark harness; none are hand-written.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="results/allocator_experiment.txt"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make bench-allocator)." >&2
    exit 1
fi

DIRTY=no
if [[ -n "$(git status --porcelain)" ]]; then DIRTY=yes; fi

{
    echo "Command:     make bench-allocator"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Build type:  Release"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Dataset:     engine::Order allocation microbenchmark (new/delete vs fixed pool)"
    echo "Warmup:      iters/10 per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
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

#!/usr/bin/env bash
# Run the benchmark harness (release build) and write a results file with full metadata.
# All numbers are produced here; none are hand-written. Results are hardware-dependent.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="build/release/qsl-bench"
OUT="results/latest.txt"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the release preset first (make bench)." >&2
    exit 1
fi

DIRTY=no
if [[ -n "$(git status --porcelain)" ]]; then DIRTY=yes; fi

{
    echo "Command:     make bench"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Build type:  Release"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Dataset:     synthetic order flow (replay::generate_flow, seed 42, 4 symbols)"
    echo "Warmup:      iters/10 (or 1 throughput pass) per benchmark, before timing"
    echo "Units:       latency = ns/op + ops/sec; throughput = ns/item + items/sec"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
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

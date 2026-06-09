#!/usr/bin/env bash
# Run the M44 benchmark-only packed-vs-padded SPSC cursor false-sharing study.
# All numbers are produced by qsl-bench; none are hand-written.
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/qsl_common.sh

BIN="${QSL_FALSE_SHARING_BIN:-${QSL_BENCH_BIN:-build/bench/qsl-bench}}"
OUT="${QSL_FALSE_SHARING_OUT:-results/false_sharing_study.txt}"
DATASET="${QSL_FALSE_SHARING_DATASET:-synthetic SPSC cursor exchange (producer tail / consumer head)}"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make false-sharing-study)." >&2
    exit 1
fi

DIRTY="$(qsl_dirty_tree_status "$OUT")"

{
    echo "Command:        make false-sharing-study"
    echo "Evidence class: research-notes"
    echo "Hardware:       $(uname -m)"
    echo "CPU:            $(qsl_cpu_model)"
    echo "OS:             $(uname -s) $(uname -r)"
    echo "Compiler:       $(qsl_build_compiler_version build/bench)"
    echo "Build type:     $(qsl_build_type build/bench)"
    echo "Git commit:     $(qsl_git_commit_short)"
    echo "Dataset:        $DATASET"
    echo "Dirty tree:     $DIRTY (excluding this generated output)"
    echo "Artifact provenance: source commit excludes only this generated output; rerun from that commit to reproduce the benchmark payload."
    echo "Output excluded from dirty-tree check: $OUT"
    echo "Date:           $(qsl_utc_timestamp)"
    echo "Output path:    $OUT"
    echo
    echo "Host support summary: portable two-thread C++ benchmark; no PMU counters required."
    echo "Scenario: benchmark-only SPSC queue cursor layout, packed indices vs cache-line-padded indices."
    echo "Memory ordering: owner stores use release; peer observations use acquire, matching the"
    echo "production SpscRing cursor hand-off pattern."
    echo "Production impact: none; this study does not change SpscRing layout or matching ownership."
    echo "Caveat: cache-line contention measurements are scheduler-, CPU-, OS-, and topology-dependent;"
    echo "do not report these numbers as production latency or general speedup evidence."
    echo
    echo "Scenario / Metric / Result:"
    "$BIN" false-sharing
} >"$OUT"

echo "wrote $OUT"
cat "$OUT"

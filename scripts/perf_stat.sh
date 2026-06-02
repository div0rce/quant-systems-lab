#!/usr/bin/env bash
# Run Linux perf stat against the benchmark harness and write metadata-rich output.
# Hardware counters require a Linux kernel/host that exposes PMU events to the process.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_BENCH_BIN:-build/bench/qsl-bench}"
OUT="${QSL_PERF_STAT_OUT:-results/perf_stat_linux.txt}"
EVENTS="${QSL_PERF_STAT_EVENTS:-cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,page-faults}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd -P)"

repo_relative_or_empty() {
    local path="$1"
    local abs
    local dir
    local base
    local resolved_dir
    local resolved

    if [[ -z "$path" ]]; then
        return
    fi
    if [[ "$path" = /* ]]; then
        abs="$path"
    else
        abs="$REPO_ROOT/$path"
    fi

    dir="$(dirname "$abs")"
    base="$(basename "$abs")"
    if ! resolved_dir="$(cd "$dir" 2>/dev/null && pwd -P)"; then
        return
    fi
    resolved="$resolved_dir/$base"

    case "$resolved" in
    "$REPO_ROOT"/*) printf '%s\n' "${resolved#"$REPO_ROOT"/}" ;;
    esac
}

dirty_tree_status() {
    local out_rel
    local status_output
    local pathspecs=(. ":(exclude)results/perf_stat_linux.txt"
        ":(exclude)results/perf_report_linux.txt")

    out_rel="$(repo_relative_or_empty "$OUT")"
    if [[ -n "$out_rel" ]]; then
        pathspecs+=(":(exclude)$out_rel")
    fi

    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-tree check failed; refusing to write misleading metadata." >&2
        exit 2
    fi

    if [[ -n "$status_output" ]]; then
        echo yes
    else
        echo no
    fi
}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: scripts/perf_stat.sh requires Linux perf; current OS is $(uname -s)." >&2
    exit 2
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found. Install linux perf tooling for this kernel." >&2
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make perf-stat)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

DIRTY="$(dirty_tree_status)"

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
        echo "CPU:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
        echo "Compiler:    $(c++ --version | head -1)"
        echo "Perf:        $(perf --version)"
        echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
        echo "Build type:  Release"
        echo "Git commit:  $(git rev-parse --short HEAD)"
        echo "Dirty tree:  $DIRTY"
        echo "Benchmark binary: $BIN"
        echo "Benchmark status: $BENCH_STATUS"
        echo "Dataset:     qsl-bench default synthetic benchmark suite"
        echo "Events:      $EVENTS"
        echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo
        echo "Benchmark output:"
        cat "$BENCH_OUT"
    } >"$TMP_OUT"
    mv "$TMP_OUT" "$OUT"
    echo "wrote $OUT"
    cat "$OUT"
    echo "error: benchmark command failed before perf stat; partial mode cannot override this." >&2
    exit 4
fi

PERF_STATUS=0
perf stat -e "$EVENTS" -- "$BIN" >"$PERF_BENCH_OUT" 2>"$PERF_OUT" || PERF_STATUS=$?

UNSUPPORTED=no
if grep -Eiq '<not supported>|not supported|No permission|not counted|Operation not permitted|Permission denied' "$PERF_OUT"; then
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
    echo "CPU:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Perf:        $(perf --version)"
    echo "Perf paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
    echo "Build type:  Release"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Benchmark binary: $BIN"
    echo "Benchmark status: $BENCH_STATUS"
    echo "Dataset:     qsl-bench default synthetic benchmark suite"
    echo "Events:      $EVENTS"
    echo "Perf status: $PERF_STATUS"
    echo "Unsupported counters detected: $UNSUPPORTED"
    echo "Hardware counters supported: $HARDWARE_COUNTERS_SUPPORTED"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
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

mv "$TMP_OUT" "$OUT"
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

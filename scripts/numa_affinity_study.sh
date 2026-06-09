#!/usr/bin/env bash
# Run a Linux CPU-affinity / scheduler-migration / NUMA locality study.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

BIN="${QSL_NUMA_BIN:-build/bench/qsl-bench}"
OUT="${QSL_NUMA_OUT:-results/numa_affinity_study.txt}"
ALLOW_CONSTRAINED="${QSL_NUMA_ALLOW_CONSTRAINED:-0}"
PERF_EVENTS="context-switches,cpu-migrations"

shell_quote() {
    printf '%q' "$1"
}

recorded_command() {
    local -a env_parts=()
    if [[ -n "${QSL_NUMA_ALLOW_CONSTRAINED+x}" ]]; then
        env_parts+=("QSL_NUMA_ALLOW_CONSTRAINED=$(shell_quote "$ALLOW_CONSTRAINED")")
    fi
    if [[ -n "${QSL_NUMA_CPU+x}" ]]; then
        env_parts+=("QSL_NUMA_CPU=$(shell_quote "$QSL_NUMA_CPU")")
    fi
    if [[ -n "${QSL_NUMA_OUT+x}" ]]; then
        env_parts+=("QSL_NUMA_OUT=$(shell_quote "$OUT")")
    fi
    if [[ -n "${QSL_NUMA_BIN+x}" ]]; then
        env_parts+=("QSL_NUMA_BIN=$(shell_quote "$BIN")")
    fi
    if ((${#env_parts[@]} > 0)); then
        printf '%s ' "${env_parts[@]}"
    fi
    printf 'make numa-study\n'
}

if [[ "$ALLOW_CONSTRAINED" != "0" && "$ALLOW_CONSTRAINED" != "1" ]]; then
    echo "error: QSL_NUMA_ALLOW_CONSTRAINED must be 0 or 1" >&2
    exit 2
fi

if [[ -n "${QSL_NUMA_CPU:-}" && ! "$QSL_NUMA_CPU" =~ ^[0-9]+$ ]]; then
    echo "error: QSL_NUMA_CPU must be an integer" >&2
    exit 2
fi

COMMAND_LINE="$(recorded_command)"

mkdir -p "$(dirname "$OUT")"
DIRTY="$(qsl_dirty_tree_status "$OUT")"

TMP_OUT="$(mktemp)"
UNPINNED_OUT="$(mktemp)"
PINNED_OUT="$(mktemp)"
PERF_UNPINNED_OUT="$(mktemp)"
PERF_PINNED_OUT="$(mktemp)"
NUMA_LOCAL_OUT="$(mktemp)"
NUMA_REMOTE_OUT="$(mktemp)"
LSCPU_OUT="$(mktemp)"
NUMACTL_OUT="$(mktemp)"
trap 'rm -f "$TMP_OUT" "$UNPINNED_OUT" "$PINNED_OUT" "$PERF_UNPINNED_OUT" "$PERF_PINNED_OUT" "$NUMA_LOCAL_OUT" "$NUMA_REMOTE_OUT" "$LSCPU_OUT" "$NUMACTL_OUT"' EXIT

first_cpu_from_list() {
    local list="$1" part cpu
    local -a parts
    IFS=',' read -r -a parts <<<"$list"
    for part in "${parts[@]}"; do
        cpu="${part%%-*}"
        if [[ "$cpu" =~ ^[0-9]+$ ]]; then
            printf '%s\n' "$cpu"
            return 0
        fi
    done
    return 1
}

allowed_cpu_list() {
    awk -F: '/^Cpus_allowed_list:/ { gsub(/^[[:space:]]+/, "", $2); print $2; exit }' /proc/self/status
}

numa_node_count() {
    if command -v lscpu >/dev/null 2>&1; then
        lscpu 2>/dev/null | awk -F: '/^NUMA node\(s\):/ { gsub(/^[[:space:]]+/, "", $2); print $2; exit }'
    fi
}

numa_node_ids() {
    awk '/^node [0-9]+ cpus:/ { print $2 }' "$NUMACTL_OUT"
}

perf_counter_captured() {
    local file="$1" counter="$2"
    grep -E "[[:space:]]$counter([[:space:]]|$)" "$file" |
        grep -Eiv '<not supported>|not supported|not counted|No permission|Operation not permitted|Permission denied' >/dev/null 2>&1
}

write_unsupported_artifact() {
    {
        echo "Command:     $COMMAND_LINE"
        echo "Evidence class: unsupported-host"
        echo "Host support summary: unsupported OS for Linux CPU-affinity / NUMA study"
        echo "Hardware:    $(uname -m)"
        echo "OS:          $(uname -s) $(uname -r)"
        echo "CPU:         $(qsl_cpu_model)"
        echo "Compiler:    $(qsl_compiler_version)"
        echo "Build type:  $(qsl_build_type build/bench)"
        echo "Git commit:  $(qsl_git_commit_short)"
        echo "Dirty tree:  $DIRTY"
        echo "Benchmark binary: $BIN"
        echo "CPU chosen:  none"
        echo "taskset available: no"
        echo "taskset succeeded: no"
        echo "perf available: no"
        echo "perf ran:     no"
        echo "context-switches captured: no"
        echo "cpu-migrations captured: no"
        echo "numactl topology available: no"
        echo "Date:        $(qsl_utc_timestamp)"
        echo
        echo "Caveat: This host is not Linux, so no CPU-affinity, scheduler-migration, or NUMA evidence was collected."
    } >"$TMP_OUT"
    mv "$TMP_OUT" "$OUT"
    echo "wrote $OUT"
    cat "$OUT"
    exit 2
}

if [[ "$(uname -s)" != "Linux" ]]; then
    write_unsupported_artifact
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the benchmark preset first (make numa-study)." >&2
    exit 1
fi

LSCPU_AVAILABLE=no
if command -v lscpu >/dev/null 2>&1 && lscpu >"$LSCPU_OUT" 2>&1; then
    LSCPU_AVAILABLE=yes
fi

NUMACTL_TOPOLOGY_AVAILABLE=no
if command -v numactl >/dev/null 2>&1 && numactl --hardware >"$NUMACTL_OUT" 2>&1; then
    NUMACTL_TOPOLOGY_AVAILABLE=yes
fi

NUMA_NODES="$(numa_node_count || true)"
[[ -z "$NUMA_NODES" ]] && NUMA_NODES="unknown"

ALLOWED_CPUS="$(allowed_cpu_list || true)"
[[ -z "$ALLOWED_CPUS" ]] && ALLOWED_CPUS="unknown"

CPU_CHOSEN="${QSL_NUMA_CPU:-}"
if [[ -z "$CPU_CHOSEN" && "$ALLOWED_CPUS" != "unknown" ]]; then
    CPU_CHOSEN="$(first_cpu_from_list "$ALLOWED_CPUS" || true)"
fi
[[ -z "$CPU_CHOSEN" ]] && CPU_CHOSEN="0"

TASKSET_AVAILABLE=no
TASKSET_SUCCEEDED=no
if command -v taskset >/dev/null 2>&1; then
    TASKSET_AVAILABLE=yes
fi

UNPINNED_STATUS=0
"$BIN" >"$UNPINNED_OUT" 2>&1 || UNPINNED_STATUS=$?

PINNED_STATUS=127
if [[ "$TASKSET_AVAILABLE" == "yes" ]]; then
    PINNED_STATUS=0
    taskset -c "$CPU_CHOSEN" "$BIN" >"$PINNED_OUT" 2>&1 || PINNED_STATUS=$?
    if [[ "$PINNED_STATUS" -eq 0 ]]; then
        TASKSET_SUCCEEDED=yes
    fi
else
    echo "taskset not available" >"$PINNED_OUT"
fi

BENCHMARKS_COMPLETED=yes
if [[ "$UNPINNED_STATUS" -ne 0 ]] ||
    { [[ "$TASKSET_AVAILABLE" == "yes" ]] && [[ "$PINNED_STATUS" -ne 0 ]]; }; then
    BENCHMARKS_COMPLETED=no
fi

FIRST_NUMA_NODE=unknown
SECOND_NUMA_NODE=unknown
NUMA_BINDING_SUCCEEDED=no
NUMA_LOCAL_STATUS=127
NUMA_REMOTE_STATUS=127
mapfile -t NUMA_NODE_IDS < <(numa_node_ids || true)
if [[ "$NUMACTL_TOPOLOGY_AVAILABLE" == "yes" && "${#NUMA_NODE_IDS[@]}" -ge 2 ]]; then
    FIRST_NUMA_NODE="${NUMA_NODE_IDS[0]}"
    SECOND_NUMA_NODE="${NUMA_NODE_IDS[1]}"
    NUMA_LOCAL_STATUS=0
    numactl --cpunodebind="$FIRST_NUMA_NODE" --membind="$FIRST_NUMA_NODE" "$BIN" \
        >"$NUMA_LOCAL_OUT" 2>&1 || NUMA_LOCAL_STATUS=$?
    NUMA_REMOTE_STATUS=0
    numactl --cpunodebind="$FIRST_NUMA_NODE" --membind="$SECOND_NUMA_NODE" "$BIN" \
        >"$NUMA_REMOTE_OUT" 2>&1 || NUMA_REMOTE_STATUS=$?
    if [[ "$NUMA_LOCAL_STATUS" -eq 0 && "$NUMA_REMOTE_STATUS" -eq 0 ]]; then
        NUMA_BINDING_SUCCEEDED=yes
    fi
elif [[ "$NUMACTL_TOPOLOGY_AVAILABLE" == "yes" ]]; then
    echo "NUMA node-local/remote binding skipped: fewer than two NUMA nodes found" >"$NUMA_LOCAL_OUT"
    echo "NUMA node-local/remote binding skipped: fewer than two NUMA nodes found" >"$NUMA_REMOTE_OUT"
else
    echo "NUMA node-local/remote binding skipped: numactl topology unavailable" >"$NUMA_LOCAL_OUT"
    echo "NUMA node-local/remote binding skipped: numactl topology unavailable" >"$NUMA_REMOTE_OUT"
fi

PERF_AVAILABLE=no
PERF_RAN=no
PERF_UNPINNED_STATUS=127
PERF_PINNED_STATUS=127
UNPINNED_CONTEXT_SWITCHES_CAPTURED=no
UNPINNED_CPU_MIGRATIONS_CAPTURED=no
PINNED_CONTEXT_SWITCHES_CAPTURED=no
PINNED_CPU_MIGRATIONS_CAPTURED=no
CONTEXT_SWITCHES_CAPTURED=no
CPU_MIGRATIONS_CAPTURED=no
if command -v perf >/dev/null 2>&1; then
    PERF_AVAILABLE=yes
    PERF_RAN=yes
    PERF_UNPINNED_STATUS=0
    perf stat -e "$PERF_EVENTS" -- "$BIN" >/dev/null 2>"$PERF_UNPINNED_OUT" || PERF_UNPINNED_STATUS=$?
    if [[ "$TASKSET_SUCCEEDED" == "yes" ]]; then
        PERF_PINNED_STATUS=0
        perf stat -e "$PERF_EVENTS" -- taskset -c "$CPU_CHOSEN" "$BIN" >/dev/null 2>"$PERF_PINNED_OUT" || PERF_PINNED_STATUS=$?
    else
        echo "pinned perf skipped because taskset did not succeed" >"$PERF_PINNED_OUT"
    fi
    if perf_counter_captured "$PERF_UNPINNED_OUT" "context-switches"; then
        UNPINNED_CONTEXT_SWITCHES_CAPTURED=yes
    fi
    if perf_counter_captured "$PERF_UNPINNED_OUT" "cpu-migrations"; then
        UNPINNED_CPU_MIGRATIONS_CAPTURED=yes
    fi
    if perf_counter_captured "$PERF_PINNED_OUT" "context-switches"; then
        PINNED_CONTEXT_SWITCHES_CAPTURED=yes
    fi
    if perf_counter_captured "$PERF_PINNED_OUT" "cpu-migrations"; then
        PINNED_CPU_MIGRATIONS_CAPTURED=yes
    fi
fi
if [[ "$UNPINNED_CONTEXT_SWITCHES_CAPTURED" == "yes" &&
      "$PINNED_CONTEXT_SWITCHES_CAPTURED" == "yes" ]]; then
    CONTEXT_SWITCHES_CAPTURED=yes
fi
if [[ "$UNPINNED_CPU_MIGRATIONS_CAPTURED" == "yes" &&
      "$PINNED_CPU_MIGRATIONS_CAPTURED" == "yes" ]]; then
    CPU_MIGRATIONS_CAPTURED=yes
fi

EVIDENCE_CLASS="linux-constrained"
HOST_SUPPORT_SUMMARY="Linux host, constrained evidence"
if [[ "$NUMA_NODES" =~ ^[0-9]+$ && "$NUMA_NODES" -gt 1 &&
      "$NUMACTL_TOPOLOGY_AVAILABLE" == "yes" &&
      "$NUMA_BINDING_SUCCEEDED" == "yes" &&
      "$TASKSET_SUCCEEDED" == "yes" &&
      "$PERF_RAN" == "yes" &&
      "$PERF_UNPINNED_STATUS" -eq 0 &&
      "$PERF_PINNED_STATUS" -eq 0 &&
      "$CONTEXT_SWITCHES_CAPTURED" == "yes" &&
      "$CPU_MIGRATIONS_CAPTURED" == "yes" &&
      "$UNPINNED_STATUS" -eq 0 &&
      "$PINNED_STATUS" -eq 0 ]]; then
    EVIDENCE_CLASS="full-linux-numa"
    HOST_SUPPORT_SUMMARY="NUMA-capable Linux host with affinity and migration-counter evidence"
fi

{
    echo "Command:     $COMMAND_LINE"
    echo "Evidence class: $EVIDENCE_CLASS"
    echo "Host support summary: $HOST_SUPPORT_SUMMARY"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(qsl_cpu_model)"
    echo "Compiler:    $(qsl_compiler_version)"
    echo "Build type:  $(qsl_build_type build/bench)"
    echo "Git commit:  $(qsl_git_commit_short)"
    echo "Dirty tree:  $DIRTY"
    echo "Benchmark binary: $BIN"
    echo "Allowed CPUs: $ALLOWED_CPUS"
    echo "CPU chosen:  $CPU_CHOSEN"
    echo "NUMA nodes:  $NUMA_NODES"
    echo "lscpu topology available: $LSCPU_AVAILABLE"
    echo "numactl topology available: $NUMACTL_TOPOLOGY_AVAILABLE"
    echo "NUMA node-local/remote binding succeeded: $NUMA_BINDING_SUCCEEDED"
    echo "NUMA local node: $FIRST_NUMA_NODE"
    echo "NUMA remote node: $SECOND_NUMA_NODE"
    echo "taskset available: $TASKSET_AVAILABLE"
    echo "taskset succeeded: $TASKSET_SUCCEEDED"
    echo "perf available: $PERF_AVAILABLE"
    echo "perf ran:     $PERF_RAN"
    echo "perf events:  $PERF_EVENTS"
    echo "context-switches captured: $CONTEXT_SWITCHES_CAPTURED"
    echo "cpu-migrations captured: $CPU_MIGRATIONS_CAPTURED"
    echo "unpinned context-switches captured: $UNPINNED_CONTEXT_SWITCHES_CAPTURED"
    echo "pinned context-switches captured: $PINNED_CONTEXT_SWITCHES_CAPTURED"
    echo "unpinned cpu-migrations captured: $UNPINNED_CPU_MIGRATIONS_CAPTURED"
    echo "pinned cpu-migrations captured: $PINNED_CPU_MIGRATIONS_CAPTURED"
    echo "Benchmark runs completed:  $BENCHMARKS_COMPLETED"
    echo "Unpinned benchmark status: $UNPINNED_STATUS"
    echo "Pinned benchmark status:   $PINNED_STATUS"
    echo "NUMA local benchmark status:  $NUMA_LOCAL_STATUS"
    echo "NUMA remote benchmark status: $NUMA_REMOTE_STATUS"
    echo "Unpinned perf status:      $PERF_UNPINNED_STATUS"
    echo "Pinned perf status:        $PERF_PINNED_STATUS"
    echo "Date:        $(qsl_utc_timestamp)"
    echo
    echo "Caveat: CPU-affinity and NUMA measurements are host-specific systems evidence, not a production-latency or speedup claim."
    echo
    echo "Unpinned command:"
    echo "$BIN"
    echo
    echo "Pinned command:"
    echo "taskset -c $CPU_CHOSEN $BIN"
    echo
    echo "Unpinned benchmark output:"
    cat "$UNPINNED_OUT"
    echo
    echo "Pinned benchmark output:"
    cat "$PINNED_OUT"
    echo
    echo "NUMA local benchmark output:"
    cat "$NUMA_LOCAL_OUT"
    echo
    echo "NUMA remote benchmark output:"
    cat "$NUMA_REMOTE_OUT"
    echo
    echo "Unpinned perf stat output:"
    cat "$PERF_UNPINNED_OUT"
    echo
    echo "Pinned perf stat output:"
    cat "$PERF_PINNED_OUT"
    echo
    echo "lscpu output:"
    cat "$LSCPU_OUT"
    echo
    echo "numactl --hardware output:"
    cat "$NUMACTL_OUT"
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

if [[ "$BENCHMARKS_COMPLETED" != "yes" ]]; then
    echo "error: NUMA study benchmark runs did not complete successfully." >&2
    echo "       Refusing to treat the artifact as valid constrained evidence." >&2
    exit 1
fi

if [[ "$EVIDENCE_CLASS" == "linux-constrained" && "$ALLOW_CONSTRAINED" != "1" ]]; then
    echo "error: NUMA study produced constrained Linux evidence." >&2
    echo "       Re-run on a NUMA-capable Linux host with taskset/perf support, or set QSL_NUMA_ALLOW_CONSTRAINED=1" >&2
    echo "       only when intentionally documenting a constrained environment." >&2
    exit 3
fi

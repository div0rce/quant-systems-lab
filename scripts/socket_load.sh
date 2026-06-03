#!/usr/bin/env bash
# Multi-client TCP load / connection-scaling test for the order gateway.
#
# Drives N concurrent short-lived clients (qsl-client: connect, NewOrder + Heartbeat, read, close)
# against qsl-gateway in BOTH transport modes -- the blocking single-connection accept loop (M9)
# and the epoll event loop (M34) -- and reports how each scales with the client count.
#
# What this isolates: the per-request engine work (matching one order) is sub-microsecond, so the
# wall time is dominated by connection setup/accept and the socket I/O path. Loopback only,
# constrained-environment: this is NOT a production capacity or connections/sec claim -- absolute
# numbers are hardware/kernel/load dependent. The point is the scaling trend per mode; at this
# small loopback scale the two modes may stay close because connection setup dominates. A clear
# epoll advantage would require higher concurrency or heavier per-connection work.
#
# Reliability: every gateway is started on a FRESH, never-reused port (a monotonic allocator), so a
# port still in TIME_WAIT from an earlier cell can never make a bind fail mid-sweep. If a gateway
# still fails to start (a transient), the trial retries on the next fresh port up to
# QSL_LOAD_MAX_ATTEMPTS times. If a cell still cannot reach full completion, the script writes no
# artifact and exits nonzero (set QSL_LOAD_ALLOW_PARTIAL=1 to record a partial run intentionally) --
# a flaky benchmark artifact is worse than none.
#
# Linux-only (the epoll mode and the high-resolution `date +%s.%N` timer); skips clearly elsewhere,
# like scripts/profile_gateway_io.sh.
set -euo pipefail

cd "$(dirname "$0")/.."

GATEWAY="${QSL_GATEWAY_BIN:-build/dev/qsl-gateway}"
CLIENT="${QSL_CLIENT_BIN:-build/dev/qsl-client}"
OUT="${QSL_SOCKET_LOAD_OUT:-results/socket_load_summary.txt}"
PORT_BASE="${QSL_LOAD_PORT:-39300}"
CLIENT_COUNTS="${QSL_LOAD_COUNTS:-1 8 32 64}"
TRIALS="${QSL_LOAD_TRIALS:-3}"
CLIENT_TIMEOUT="${QSL_LOAD_CLIENT_TIMEOUT:-30}" # per-client wall cap; bounds a hang if the gateway dies
MAX_ATTEMPTS="${QSL_LOAD_MAX_ATTEMPTS:-6}"      # retries (on a fresh port) before a trial is failed

REPO_ROOT="$(git rev-parse --show-toplevel)"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd -P)"

repo_relative_or_empty() {
    local path="$1" abs dir base resolved_dir resolved
    [[ -z "$path" ]] && return
    if [[ "$path" = /* ]]; then abs="$path"; else abs="$REPO_ROOT/$path"; fi
    dir="$(dirname "$abs")"
    base="$(basename "$abs")"
    resolved_dir="$(cd "$dir" 2>/dev/null && pwd -P)" || return
    resolved="$resolved_dir/$base"
    case "$resolved" in
    "$REPO_ROOT"/*) printf '%s\n' "${resolved#"$REPO_ROOT"/}" ;;
    esac
}

dirty_tree_status() {
    local out_rel status_output
    local pathspecs=(. ":(exclude)results/socket_load_summary.txt")
    out_rel="$(repo_relative_or_empty "$OUT")"
    [[ -n "$out_rel" ]] && pathspecs+=(":(exclude)$out_rel")
    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-tree check failed; refusing to write misleading metadata." >&2
        exit 2
    fi
    if [[ -n "$status_output" ]]; then echo yes; else echo no; fi
}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: scripts/socket_load.sh requires Linux (epoll mode + high-res timer); current OS is $(uname -s)." >&2
    echo "       Run it on a Linux host or inside a Linux container to generate the artifact." >&2
    exit 2
fi
if [[ ! -x "$GATEWAY" || ! -x "$CLIENT" ]]; then
    echo "error: gateway/client binaries not found; build the dev preset first (make build)." >&2
    exit 1
fi
if ! [[ "$TRIALS" =~ ^[0-9]+$ ]] || ((TRIALS < 1)); then
    echo "error: QSL_LOAD_TRIALS must be a positive integer (got '$TRIALS')." >&2
    exit 2
fi
if ! [[ "$MAX_ATTEMPTS" =~ ^[0-9]+$ ]] || ((MAX_ATTEMPTS < 1)); then
    echo "error: QSL_LOAD_MAX_ATTEMPTS must be a positive integer (got '$MAX_ATTEMPTS')." >&2
    exit 2
fi

mkdir -p "$(dirname "$OUT")"
DIRTY="$(dirty_tree_status)"

NEXT_PORT="$PORT_BASE" # monotonic: every gateway gets a brand-new port, never reused this run
GW_PID=""
RESULT_WALL=""
RESULT_OKRATIO=""
trap 'if [[ -n "$GW_PID" ]]; then kill -KILL "$GW_PID" 2>/dev/null || true; fi' EXIT

now() { date +%s.%N; }

wait_ready() {
    local port="$1" pid="$2" i
    for i in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || return 1 # gateway exited (e.g. bind failed) -> not ready
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.05
    done
    return 1
}

stop_gateway() {
    [[ -z "$GW_PID" ]] && return 0
    local i
    kill -INT "$GW_PID" 2>/dev/null || true
    for ((i = 0; i < 20; i++)); do
        kill -0 "$GW_PID" 2>/dev/null || break
        sleep 0.05
    done
    kill -KILL "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
    GW_PID=""
}

# Run one trial: start the gateway in $mode on a fresh port, fire N concurrent clients, measure.
# Retries on a new fresh port if the gateway fails to start. Sets RESULT_WALL / RESULT_OKRATIO.
# (Runs in the current shell -- not a subshell -- so NEXT_PORT/GW_PID stay consistent.)
run_load() {
    local mode="$1" n="$2" attempt port start end ok c p
    for ((attempt = 0; attempt < MAX_ATTEMPTS; attempt++)); do
        port="$NEXT_PORT"
        NEXT_PORT=$((NEXT_PORT + 1))
        local gw_args=("$port")
        [[ "$mode" == "epoll" ]] && gw_args+=("--epoll")

        "$GATEWAY" "${gw_args[@]}" >/dev/null 2>&1 &
        GW_PID=$!
        if ! wait_ready "$port" "$GW_PID"; then
            stop_gateway # gateway failed to start (transient bind, etc.) -> retry on a fresh port
            continue
        fi

        local pids=()
        ok=0
        start="$(now)"
        for ((c = 0; c < n; c++)); do
            timeout "$CLIENT_TIMEOUT" "$CLIENT" "$port" >/dev/null 2>&1 &
            pids+=("$!")
        done
        for p in "${pids[@]}"; do
            if wait "$p"; then ok=$((ok + 1)); fi
        done
        end="$(now)"
        stop_gateway

        if ((ok == n)); then
            RESULT_WALL="$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.4f", e - s }')"
            RESULT_OKRATIO="$n/$n"
            return 0
        fi
        # A served gateway that still dropped clients is unexpected (the gateway is reliable in
        # isolation); retry the whole trial rather than record a flaky cell.
    done
    RESULT_WALL="0"
    RESULT_OKRATIO="0/$n"
    return 1
}

# Best (minimum) wall over the successful trials + the worst (fewest-completed) ratio across trials.
# A failed trial never poisons the minimum, and a failure is surfaced in the worst ratio. Sets
# BEST_WALL / WORST_OKRATIO.
best_of() {
    local mode="$1" n="$2" t ok
    BEST_WALL=""
    WORST_OKRATIO=""
    for ((t = 0; t < TRIALS; t++)); do
        run_load "$mode" "$n" || true
        ok="${RESULT_OKRATIO%%/*}"
        if [[ -z "$WORST_OKRATIO" ]] || ((ok < ${WORST_OKRATIO%%/*})); then
            WORST_OKRATIO="$RESULT_OKRATIO"
        fi
        if ((ok > 0)); then
            if [[ -z "$BEST_WALL" ]] || awk -v a="$RESULT_WALL" -v b="$BEST_WALL" 'BEGIN { exit !(a < b) }'; then
                BEST_WALL="$RESULT_WALL"
            fi
        fi
    done
    [[ -z "$BEST_WALL" ]] && BEST_WALL="0"
    [[ -z "$WORST_OKRATIO" ]] && WORST_OKRATIO="0/$n"
    return 0
}

declare -a ROWS=()
for mode in blocking epoll; do
    for n in $CLIENT_COUNTS; do
        best_of "$mode" "$n"
        rate="$(awk -v n="$n" -v w="$BEST_WALL" 'BEGIN { if (w > 0) printf "%.0f", n / w; else printf "n/a" }')"
        ROWS+=("$mode|$n|$BEST_WALL|$rate|$WORST_OKRATIO")
    done
done

# Fail loud on any incomplete cell: a flaky benchmark artifact is worse than none.
INCOMPLETE=0
for row in "${ROWS[@]}"; do
    IFS='|' read -r _m _n _w _r okr <<<"$row"
    [[ "${okr%%/*}" != "${okr##*/}" ]] && INCOMPLETE=$((INCOMPLETE + 1))
done
if ((INCOMPLETE > 0)) && [[ "${QSL_LOAD_ALLOW_PARTIAL:-0}" != "1" ]]; then
    {
        echo "error: $INCOMPLETE load cell(s) did not reach full completion after $MAX_ATTEMPTS attempts."
        echo "       Refusing to write an unreliable artifact. Rows:"
        printf '         %s\n' "${ROWS[@]}"
        echo "       Investigate, or set QSL_LOAD_ALLOW_PARTIAL=1 to record a partial run intentionally."
    } >&2
    exit 3
fi

TMP_OUT="$(mktemp)"
{
    echo "Command:     bash scripts/socket_load.sh  (make socket-load)"
    echo "Artifact:    multi-client TCP connection-scaling load (loopback, constrained)"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
    echo "Cores:       $(nproc 2>/dev/null || echo unknown)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Transport:   TCP over 127.0.0.1 (loopback)"
    echo "Load shape:  N concurrent qsl-client connections; each connects, sends NewOrder + Heartbeat, reads replies, closes"
    echo "Client counts: $CLIENT_COUNTS"
    echo "Trials/cell: $TRIALS (best/min wall over trials; each gateway on a fresh port, up to $MAX_ATTEMPTS start attempts)"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Setup: the same concurrent client load is run against the gateway in each transport mode."
    echo "blocking = the M9 single-connection accept loop (serves one connection at a time); epoll ="
    echo "the M34 event loop (multiplexes readiness across all connections). Per-order engine work is"
    echo "sub-microsecond, so the wall time is the connection-setup/accept/socket path, not matching."
    echo
    printf '%-9s %8s %14s %14s %10s\n' "mode" "clients" "wall(s,best)" "conns/s(~)" "completed"
    printf '%-9s %8s %14s %14s %10s\n' "-------" "-------" "------------" "----------" "---------"
    for row in "${ROWS[@]}"; do
        IFS='|' read -r mode n wall rate okratio <<<"$row"
        printf '%-9s %8s %14s %14s %10s\n' "$mode" "$n" "$wall" "$rate" "$okratio"
    done
    echo
    echo "Reading the result: compare how the best wall time grows with the client count within each"
    echo "mode. In principle the blocking server (one connection at a time) should scale worse than"
    echo "epoll (which multiplexes), but at these small loopback counts connection setup dominates and"
    echo "the two modes stay close (same order of magnitude); which is marginally faster varies run to"
    echo "run. A clear epoll advantage would require higher concurrency or heavier per-connection work."
    echo "Absolute conns/s figures are loopback, single-machine, and load dependent -- not a"
    echo "production-capacity or throughput claim. Every 'completed' cell is N/N: a load run that"
    echo "cannot reach full completion writes no artifact and fails, so this table is never partial"
    echo "unless QSL_LOAD_ALLOW_PARTIAL=1 was set."
    echo
    echo "Caveats:"
    echo "- Loopback only: no NIC, driver, routing, or real-network behaviour is exercised."
    echo "- Each client is a brand-new short-lived connection (connection-setup heavy), not a"
    echo "  long-lived high-throughput session; this measures connection scaling, not steady-state"
    echo "  message throughput."
    echo "- Spawning client processes adds fork/exec cost on the driver side; both modes pay it"
    echo "  equally, so the blocking-vs-epoll comparison is still meaningful, but absolute conns/s"
    echo "  is bounded by client spawn cost, not just the server."
    echo "- Constrained-environment evidence for investigation; not a production-capacity claim."
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

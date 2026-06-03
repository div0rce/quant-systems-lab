#!/usr/bin/env bash
# Multi-client TCP load / connection-scaling test for the order gateway.
#
# Drives N concurrent short-lived clients (qsl-client: connect, NewOrder + Heartbeat, read, close)
# against qsl-gateway in BOTH transport modes -- the blocking single-connection accept loop (M9)
# and the epoll event loop (M34) -- and reports how each scales with the client count.
#
# What this isolates: the per-request engine work (matching one order) is sub-microsecond, so the
# wall time is dominated by connection setup/accept and the socket I/O path. The blocking server
# serves one connection at a time (so concurrent clients serialize), while the epoll server
# multiplexes readiness across all of them; the contrast is the transport architecture, not engine
# cost. Loopback only, constrained-environment: this is NOT a production capacity or
# connections/sec claim -- absolute numbers are hardware/kernel/load dependent. The point is the
# scaling trend per mode; at small loopback connection counts (where per-connection serve work is
# tiny and connection setup dominates) the two modes look similar, and the epoll advantage only
# emerges as N rises and the blocking server's serialization starts to dominate.
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

mkdir -p "$(dirname "$OUT")"
DIRTY="$(dirty_tree_status)"

GW_PID=""
cleanup() { [[ -n "$GW_PID" ]] && kill -KILL "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT

now() { date +%s.%N; }

wait_ready() {
    local port="$1" pid="$2" i
    for i in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || return 1
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.05
    done
    return 1
}

# Start the gateway in $mode (blocking|epoll), fire N concurrent clients, measure wall time and how
# many completed. Prints: "<wall_seconds> <ok>/<n>".
run_load() {
    local mode="$1" n="$2" port="$3"
    local gw_args=("$port")
    [[ "$mode" == "epoll" ]] && gw_args+=("--epoll")

    "$GATEWAY" "${gw_args[@]}" >/dev/null 2>&1 &
    GW_PID=$!
    if ! wait_ready "$port" "$GW_PID"; then
        kill -KILL "$GW_PID" 2>/dev/null || true
        wait "$GW_PID" 2>/dev/null || true
        GW_PID=""
        echo "0 0/$n"
        return
    fi

    local start end ok=0 c
    local pids=()
    start="$(now)"
    for ((c = 0; c < n; c++)); do
        timeout "$CLIENT_TIMEOUT" "$CLIENT" "$port" >/dev/null 2>&1 &
        pids+=("$!")
    done
    for c in "${pids[@]}"; do
        if wait "$c"; then ok=$((ok + 1)); fi
    done
    end="$(now)"

    kill -INT "$GW_PID" 2>/dev/null || true
    for c in $(seq 1 20); do
        kill -0 "$GW_PID" 2>/dev/null || break
        sleep 0.05
    done
    kill -KILL "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
    GW_PID=""

    awk -v s="$start" -v e="$end" -v ok="$ok" -v n="$n" 'BEGIN { printf "%.4f %d/%d\n", e - s, ok, n }'
}

# Best (minimum) wall time over the SUCCESSFUL trials, plus the WORST (fewest-completed) ratio seen
# across all trials. A failed trial -- the gateway never bound, so run_load prints "0 0/n" -- is
# excluded from the min so it cannot masquerade as the fastest result; its failure still surfaces in
# the worst ratio. If every trial failed, best is 0 and the ratio shows 0/n (a visibly degenerate
# cell), never a clean-looking number.
best_of() {
    local mode="$1" n="$2" port="$3" t line wall okratio ok
    local best="" worst_ok=""
    for ((t = 0; t < TRIALS; t++)); do
        line="$(run_load "$mode" "$n" "$((port + t))")"
        wall="${line%% *}"
        okratio="${line##* }"
        ok="${okratio%%/*}"
        if [[ -z "$worst_ok" ]] || ((ok < ${worst_ok%%/*})); then
            worst_ok="$okratio" # the worst-completing trial, not merely the last
        fi
        if ((ok > 0)); then # only trials whose gateway actually served count toward the best wall
            if [[ -z "$best" ]] || awk -v a="$wall" -v b="$best" 'BEGIN { exit !(a < b) }'; then
                best="$wall"
            fi
        fi
    done
    [[ -z "$best" ]] && best="0"
    [[ -z "$worst_ok" ]] && worst_ok="0/$n"
    printf '%s %s\n' "$best" "$worst_ok"
}

declare -a ROWS=()
port="$PORT_BASE"
for mode in blocking epoll; do
    for n in $CLIENT_COUNTS; do
        res="$(best_of "$mode" "$n" "$port")"
        wall="${res%% *}"
        okratio="${res##* }"
        rate="$(awk -v n="$n" -v w="$wall" 'BEGIN { if (w > 0) printf "%.0f", n / w; else printf "n/a" }')"
        ROWS+=("$mode|$n|$wall|$rate|$okratio")
        port=$((port + 100))
    done
done

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
    echo "Trials/cell: $TRIALS (best/min wall time reported)"
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
    echo "Reading the result: compare how the best wall time grows with the client count within"
    echo "each mode. The blocking server serializes concurrent connections while epoll multiplexes"
    echo "them, so epoll should grow more slowly as N rises -- but at these small loopback counts,"
    echo "where per-connection serve work is tiny and connection setup dominates, the two modes can"
    echo "look similar (same order of magnitude); the epoll advantage only emerges once"
    echo "serialization dominates. Absolute conns/s figures are loopback, single-machine, and load"
    echo "dependent -- not a production-capacity or throughput claim. The 'completed' column shows"
    echo "the worst per-trial completion; anything below N/N means a trial dropped connections."
    echo
    echo "Caveats:"
    echo "- Loopback only: no NIC, driver, routing, or real-network behaviour is exercised."
    echo "- Each client is a brand-new short-lived connection (connection-setup heavy), not a"
    echo "  long-lived high-throughput session; this measures connection scaling, not steady-state"
    echo "  message throughput."
    echo "- Spawning client processes adds fork/exec cost on the driver side; both modes pay it"
    echo "  equally, so the blocking-vs-epoll comparison is still meaningful, but absolute conns/s"
    echo "  is bounded by client spawn cost, not just the server."
    echo "- Transient: under rapid gateway start/stop cycling a trial can occasionally fail to bind"
    echo "  or serve on a constrained host, shown as a '< N/N' completed cell; the reported best"
    echo "  wall time is taken only from trials whose gateway actually served (failed trials do not"
    echo "  poison the minimum)."
    echo "- Constrained-environment evidence for investigation; not a production-capacity claim."
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

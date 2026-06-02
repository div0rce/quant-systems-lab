#!/usr/bin/env bash
# Profile the syscall / kernel-socket path of the TCP order gateway under loopback client load.
#
# Two passes drive the SAME load (CONNECTIONS sequential qsl-client round trips, each: connect,
# NewOrder + Heartbeat, read replies, close) against the gateway:
#   1. rusage pass  -> reads /proc/<pid>/{stat,status} for the gateway: user (engine-side) vs
#      system (kernel/socket) CPU time, voluntary/involuntary context switches, minor/major page
#      faults, peak RSS. Minimal perturbation (no tracer attached).
#   2. strace pass  -> attaches `strace -f -c` to the gateway for per-syscall call counts and
#      time-in-kernel, i.e. which syscalls the socket path actually spends time in. strace heavily
#      perturbs timing, so this pass is for the syscall *mix*, not wall-clock numbers.
#
# The gateway is backgrounded directly (this script owns its PID and signals it directly), so the
# stop path needs no procps/pkill and cannot hang. Linux-only (procfs + strace). Skips clearly on
# other systems. Loopback only -- no NIC/driver/routing is exercised. Constrained profiling
# evidence for investigation, not a production-latency or capacity claim.
set -euo pipefail

cd "$(dirname "$0")/.."

GATEWAY="${QSL_GATEWAY_BIN:-build/dev/qsl-gateway}"
CLIENT="${QSL_CLIENT_BIN:-build/dev/qsl-client}"
OUT="${QSL_SOCKET_PROFILE_OUT:-results/socket_profile_loopback.txt}"
PORT="${QSL_PROFILE_PORT:-39200}"
CONNECTIONS="${QSL_PROFILE_CONNECTIONS:-500}"

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
    local pathspecs=(. ":(exclude)results/socket_profile_loopback.txt"
        ":(exclude)results/socket_stress_summary.txt")
    out_rel="$(repo_relative_or_empty "$OUT")"
    [[ -n "$out_rel" ]] && pathspecs+=(":(exclude)$out_rel")
    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-tree check failed; refusing to write misleading metadata." >&2
        exit 2
    fi
    if [[ -n "$status_output" ]]; then echo yes; else echo no; fi
}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: scripts/profile_gateway_io.sh requires Linux (procfs + strace); current OS is $(uname -s)." >&2
    echo "       Run it on a Linux host or inside a Linux container to generate the artifact." >&2
    exit 2
fi
if ! command -v strace >/dev/null 2>&1; then
    echo "error: strace not found. Install strace for this kernel." >&2
    exit 2
fi
if [[ ! -x "$GATEWAY" || ! -x "$CLIENT" ]]; then
    echo "error: gateway/client binaries not found; build the dev preset first (make build)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
DIRTY="$(dirty_tree_status)"

RUSAGE_OUT="$(mktemp)"
STRACE_OUT="$(mktemp)"
STRACE_ERR="$(mktemp)"
TMP_OUT="$(mktemp)"
GW_PID=""
STRACE_PID=""
cleanup() {
    [[ -n "$STRACE_PID" ]] && kill -KILL "$STRACE_PID" 2>/dev/null || true
    [[ -n "$GW_PID" ]] && kill -KILL "$GW_PID" 2>/dev/null || true
    rm -f "$RUSAGE_OUT" "$STRACE_OUT" "$STRACE_ERR" "$TMP_OUT"
}
trap cleanup EXIT

wait_ready() {
    local port="$1" i
    for i in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.05
    done
    return 1
}

drive_load() {
    local port="$1" n="$2" i
    for ((i = 0; i < n; i++)); do
        "$CLIENT" "$port" >/dev/null 2>&1 || true
    done
}

# Stop the gateway we started directly: SIGINT (no handler -> terminate), escalate to SIGKILL so
# it can never hang, then reap.
stop_gateway() {
    local i
    [[ -z "$GW_PID" ]] && return 0
    kill -INT "$GW_PID" 2>/dev/null || true
    for i in $(seq 1 30); do
        kill -0 "$GW_PID" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
    GW_PID=""
}

# Pass 1: snapshot the gateway's rusage from procfs after the load, before stopping it.
CLK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
"$GATEWAY" "$PORT" >/dev/null 2>&1 &
GW_PID=$!
if wait_ready "$PORT"; then
    drive_load "$PORT" "$CONNECTIONS"
    stat_line="$(cat "/proc/$GW_PID/stat" 2>/dev/null || true)"
    status_blob="$(cat "/proc/$GW_PID/status" 2>/dev/null || true)"
    # /proc/<pid>/stat: strip "pid (comm) "; remaining fields start at 'state' (field 3).
    rest="${stat_line#*) }"
    read -ra F <<<"$rest"
    utime="${F[11]:-0}" # field 14
    stime="${F[12]:-0}" # field 15
    minflt="${F[7]:-0}" # field 10
    majflt="${F[9]:-0}" # field 12
    {
        awk -v u="$utime" -v s="$stime" -v c="$CLK" 'BEGIN {
            printf "User (engine-side) CPU time:   %.3f s  (%d ticks)\n", u/c, u
            printf "System (kernel/socket) CPU time: %.3f s  (%d ticks)\n", s/c, s
            tot=u+s; if (tot>0) printf "System share of CPU:           %.1f%%\n", 100.0*s/tot
        }'
        echo "Minor page faults: $minflt    Major page faults: $majflt"
        echo "$status_blob" | grep -E 'voluntary_ctxt_switches|nonvoluntary_ctxt_switches|VmHWM' || true
    } >"$RUSAGE_OUT"
else
    echo "error: gateway did not become ready for the rusage pass on port $PORT." >&2
    stop_gateway
    exit 3
fi
stop_gateway

# Pass 2: attach strace -f -c to a fresh gateway, drive the same load, then stop it so strace
# writes its summary. Uses PORT+1 to dodge TIME_WAIT on the first port.
"$GATEWAY" "$((PORT + 1))" >/dev/null 2>&1 &
GW_PID=$!
if ! wait_ready "$((PORT + 1))"; then
    echo "error: gateway did not become ready for the strace pass on port $((PORT + 1))." >&2
    stop_gateway
    exit 3
fi
strace -f -c -o "$STRACE_OUT" -p "$GW_PID" >/dev/null 2>"$STRACE_ERR" &
STRACE_PID=$!
sleep 0.5 # let strace attach before driving load
drive_load "$((PORT + 1))" "$CONNECTIONS"
stop_gateway # gateway exits -> strace detaches and writes its -c summary
STRACE_RC=0
wait "$STRACE_PID" || STRACE_RC=$?
STRACE_PID=""
# strace must have produced a real syscall summary. A nonzero strace exit or an empty/absent
# summary -- e.g. strace exists but cannot attach because ptrace is blocked by container/Yama
# policy -- is a failure, not a successful artifact with an empty syscall section. Fail loudly
# (and leave any previously committed artifact untouched) instead of writing misleading output.
if [[ "$STRACE_RC" -ne 0 ]] || ! grep -qw total "$STRACE_OUT" 2>/dev/null; then
    echo "error: strace captured no syscall summary (rc=$STRACE_RC); refusing to write a misleading artifact." >&2
    echo "       Ensure strace can attach (ptrace must be permitted for this process) and re-run." >&2
    if [[ -s "$STRACE_ERR" ]]; then
        echo "       strace stderr:" >&2
        sed 's/^/         /' "$STRACE_ERR" >&2 || true
    fi
    exit 3
fi

{
    echo "Command:     bash scripts/profile_gateway_io.sh  (make profile-io)"
    echo "Artifact:    gateway syscall / kernel-socket path profile (loopback, constrained)"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
    echo "Compiler:    $(c++ --version | head -1)"
    echo "strace:      $(strace -V 2>&1 | head -1)"
    echo "CLK_TCK:     $CLK"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Transport:   TCP over 127.0.0.1 (loopback), one connection at a time"
    echo "Load:        $CONNECTIONS sequential client round trips (NewOrder + Heartbeat each)"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "== Pass 1: gateway rusage from procfs (user vs system CPU, ctx switches, page faults) =="
    echo "User (engine-side) vs System (kernel/socket) CPU time splits user-space matching work"
    echo "from time spent in the kernel servicing accept/read/write/close on the socket path."
    echo
    cat "$RUSAGE_OUT"
    echo
    echo "== Pass 2: strace -f -c (syscall mix on the gateway socket path) =="
    echo "Call counts and in-kernel time per syscall. strace perturbs timing heavily, so read the"
    echo "syscall *mix* (which calls dominate the socket path), not the absolute seconds."
    echo
    cat "$STRACE_OUT"
    echo
    echo "Caveats:"
    echo "- Loopback only: no NIC, device driver, routing, or real-network behaviour is exercised."
    echo "- strace multiplies syscall cost; use Pass 1 (not Pass 2) for the user/kernel CPU split."
    echo "- Single-connection-at-a-time gateway (M9): this profiles that design, not a concurrent"
    echo "  server. epoll/multi-client work is intentionally out of M30 scope (see docs)."
    echo "- Constrained profiling evidence for investigation; not a production-latency claim."
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

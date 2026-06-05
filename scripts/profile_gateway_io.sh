#!/usr/bin/env bash
# Profile the syscall / kernel-socket path of the TCP order gateway under loopback client load.
#
# Two passes drive the SAME load (CONNECTIONS sequential qsl-client round trips, each: connect,
# NewOrder + Heartbeat, read replies, close) against the gateway:
#   1. rusage pass  -> reads /proc/<pid>/{stat,status} for the gateway: user (engine-side) vs
#      system (kernel/socket) CPU time, voluntary/involuntary context switches, minor/major page
#      faults, peak RSS. Minimal perturbation (no tracer attached).
#   2. strace pass  -> runs the gateway UNDER `strace -f -c` (launch form, not -p attach) for
#      per-syscall call counts and time-in-kernel, i.e. which syscalls the socket path spends time
#      in. strace heavily perturbs timing, so this pass is for the syscall *mix*, not wall-clock.
#
# Pass 1 backgrounds the gateway directly (this script owns its PID). Pass 2 launches it under
# strace so the gateway is strace's *child*: that is what lets tracing work under the common Yama
# ptrace_scope=1 default, where a tracer may only trace its own descendants (attaching to a sibling
# would need CAP_SYS_PTRACE). Both stop paths escalate to SIGKILL so the script cannot hang.
# Linux-only (procfs + strace); skips clearly on other systems. Loopback only -- no NIC/driver/
# routing is exercised. Constrained profiling evidence for investigation, not a latency claim.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

GATEWAY="${QSL_GATEWAY_BIN:-build/dev/qsl-gateway}"
CLIENT="${QSL_CLIENT_BIN:-build/dev/qsl-client}"
OUT="${QSL_SOCKET_PROFILE_OUT:-results/socket_profile_loopback.txt}"
PORT="${QSL_PROFILE_PORT:-39200}"
CONNECTIONS="${QSL_PROFILE_CONNECTIONS:-500}"

qsl_require_linux "scripts/profile_gateway_io.sh" "procfs + strace"
if ! command -v strace >/dev/null 2>&1; then
    echo "error: strace not found. Install strace for this kernel." >&2
    exit 2
fi
if [[ ! -x "$GATEWAY" || ! -x "$CLIENT" ]]; then
    echo "error: gateway/client binaries not found; build the dev preset first (make build)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
DIRTY="$(qsl_dirty_tree_status results/socket_profile_loopback.txt results/socket_stress_summary.txt "$OUT")"

RUSAGE_OUT="$(mktemp)"
STRACE_OUT="$(mktemp)"
STRACE_ERR="$(mktemp)"
TMP_OUT="$(mktemp)"
GW_PID=""
STRACE_PID=""
cleanup() {
    local kids="" k
    if [[ -n "$STRACE_PID" ]]; then
        # Kill strace's child (the traced gateway) too, so an interrupted run leaves no orphan.
        if [[ -r "/proc/$STRACE_PID/task/$STRACE_PID/children" ]]; then
            read -r kids <"/proc/$STRACE_PID/task/$STRACE_PID/children" 2>/dev/null || kids=""
            for k in $kids; do kill -KILL "$k" 2>/dev/null || true; done
        fi
        command -v pkill >/dev/null 2>&1 && pkill -KILL -P "$STRACE_PID" 2>/dev/null || true
        kill -KILL "$STRACE_PID" 2>/dev/null || true
    fi
    [[ -n "$GW_PID" ]] && kill -KILL "$GW_PID" 2>/dev/null || true
    rm -f "$RUSAGE_OUT" "$STRACE_OUT" "$STRACE_ERR" "$TMP_OUT"
}
trap cleanup EXIT

drive_load() {
    local port="$1" n="$2" i
    for ((i = 0; i < n; i++)); do
        "$CLIENT" "$port" >/dev/null 2>&1 || true
    done
}

# Stop the gateway we started directly: SIGINT (no handler -> terminate), escalate to SIGKILL so
# it can never hang, then reap.
stop_gateway() {
    [[ -z "$GW_PID" ]] && return 0
    qsl_stop_process_gracefully "$GW_PID" INT 30 0.1
    GW_PID=""
}

# Stop the gateway that strace launched, so strace observes its exit and flushes the -c summary.
# The signal must go to the gateway (strace's CHILD), never to strace: killing strace loses the
# report. SIGTERM/SIGKILL on the child both make strace report; SIGINT does not reliably terminate
# a traced child, so it is not used. kill is signalling only, so this is unaffected by Yama
# ptrace_scope. Find the child via procfs (no dependency), with pkill -P as a fallback.
stop_strace_pass() {
    local kids="" k i
    [[ -z "$STRACE_PID" ]] && return 0
    if [[ -r "/proc/$STRACE_PID/task/$STRACE_PID/children" ]]; then
        read -r kids <"/proc/$STRACE_PID/task/$STRACE_PID/children" 2>/dev/null || kids=""
    fi
    for k in $kids; do kill -TERM "$k" 2>/dev/null || true; done
    command -v pkill >/dev/null 2>&1 && pkill -TERM -P "$STRACE_PID" 2>/dev/null || true
    # strace exits once its tracee dies; wait for that (it flushes the report on the way out).
    for i in $(seq 1 20); do
        kill -0 "$STRACE_PID" 2>/dev/null || return 0
        sleep 0.1
    done
    # Child still alive -> force it (still the child, NOT strace, so the summary is preserved).
    for k in $kids; do kill -KILL "$k" 2>/dev/null || true; done
    command -v pkill >/dev/null 2>&1 && pkill -KILL -P "$STRACE_PID" 2>/dev/null || true
    for i in $(seq 1 30); do
        kill -0 "$STRACE_PID" 2>/dev/null || return 0
        sleep 0.1
    done
    # Absolute hang-guard if strace is wedged with no child left: kill it. The summary gate then
    # fails loudly rather than accepting an empty artifact.
    kill -KILL "$STRACE_PID" 2>/dev/null || true
}

# Pass 1: snapshot the gateway's rusage from procfs after the load, before stopping it.
CLK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
"$GATEWAY" "$PORT" >/dev/null 2>&1 &
GW_PID=$!
if qsl_wait_tcp_connect_ready "$PORT" "$GW_PID"; then
    drive_load "$PORT" "$CONNECTIONS"
    # Confirm our gateway is still the process on this port before recording its procfs data; a
    # dead PID here means the port was held by something else (our bind failed) or it crashed.
    if ! kill -0 "$GW_PID" 2>/dev/null; then
        echo "error: gateway is not alive before recording procfs (is port $PORT already in use, or did it crash?)." >&2
        stop_gateway
        exit 3
    fi
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

# Pass 2: run a fresh gateway UNDER strace -f -c (launch form, not -p attach) so the gateway is
# strace's child. That descendant relationship is what lets tracing work under the common Yama
# ptrace_scope=1 default; attaching to a sibling would need CAP_SYS_PTRACE. Uses PORT+1 to dodge
# TIME_WAIT on the first port.
strace -f -c -o "$STRACE_OUT" "$GATEWAY" "$((PORT + 1))" >/dev/null 2>"$STRACE_ERR" &
STRACE_PID=$!
# wait_ready watches STRACE_PID: if the traced gateway's bind fails (port in use), it exits and
# strace exits with it, so readiness fails instead of connecting to an unrelated listener.
if ! qsl_wait_tcp_connect_ready "$((PORT + 1))" "$STRACE_PID"; then
    echo "error: gateway did not become ready for the strace pass on port $((PORT + 1)) (already in use?)." >&2
    stop_strace_pass
    exit 3
fi
drive_load "$((PORT + 1))" "$CONNECTIONS"
stop_strace_pass # stop the traced gateway -> strace writes its -c summary and exits
STRACE_RC=0
wait "$STRACE_PID" 2>/dev/null || STRACE_RC=$?
STRACE_PID=""
# strace must have produced a real summary AND it must show the gateway actually serving the load
# (accept + sendto/write). A "total" row alone is not enough: if the gateway died at bind (port
# conflict) strace still records its startup syscalls, which would be a misleading "successful"
# artifact with no serving activity. The summary content is the signal, not STRACE_RC (a clean
# stop can terminate the tracee by signal, yielding a nonzero strace exit with a valid report).
if ! grep -qw total "$STRACE_OUT" 2>/dev/null \
    || ! grep -qE '(^|[[:space:]])accept' "$STRACE_OUT" 2>/dev/null \
    || ! grep -qE '(^|[[:space:]])(sendto|write)\b' "$STRACE_OUT" 2>/dev/null; then
    echo "error: strace summary lacks the gateway's serving syscalls (accept + sendto/write)." >&2
    echo "       The gateway likely did not serve the load (port $((PORT + 1)) in use, strace could" >&2
    echo "       not trace, or it crashed). Refusing to write a misleading artifact (strace rc=$STRACE_RC)." >&2
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
    echo "CPU:         $(qsl_cpu_model)"
    echo "Compiler:    $(qsl_compiler_version)"
    echo "strace:      $(strace -V 2>&1 | head -1)"
    echo "CLK_TCK:     $CLK"
    echo "Git commit:  $(qsl_git_commit_short)"
    echo "Dirty tree:  $DIRTY"
    echo "Transport:   TCP over 127.0.0.1 (loopback), one connection at a time"
    echo "Load:        $CONNECTIONS sequential client round trips (NewOrder + Heartbeat each)"
    echo "Date:        $(qsl_utc_timestamp)"
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

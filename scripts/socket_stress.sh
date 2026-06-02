#!/usr/bin/env bash
# UDP socket-pressure experiment for the market-data feed path.
#
# Sends a deterministic burst of market-data datagrams over loopback (qsl-mdfeed publish) and
# receives them with the gap-tracking client (qsl-mdfeed subscribe) under three receive-buffer
# (SO_RCVBUF) settings: a small request, the OS default, and a large request. For each setting it
# runs several trials and reports how many datagrams were published, the effective buffer the
# kernel granted, and the sequence gaps (kernel drops) detected per trial.
#
# Portable: runs on Linux and macOS (BSD sockets). UDP has no retransmit, so loss is detected,
# not recovered. Results are LOOPBACK-ONLY and timing/OS/load dependent -- they demonstrate the
# SO_RCVBUF / kernel-drop mechanism, not a production capacity or latency claim. Because loss is
# stochastic, gap counts vary between trials and between runs; that variance is the honest point.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${QSL_MDFEED_BIN:-build/dev/qsl-mdfeed}"
OUT="${QSL_SOCKET_STRESS_OUT:-results/socket_stress_summary.txt}"
PORT_BASE="${QSL_STRESS_PORT:-39100}"
SEED="${QSL_STRESS_SEED:-42}"
ORDERS="${QSL_STRESS_ORDERS:-20000}"
TRIALS="${QSL_STRESS_TRIALS:-4}"
# Receive-buffer requests in bytes: 0 means "leave the OS default". The kernel may round up
# (Linux roughly doubles) or clamp to a system maximum; the effective value is read back.
SMALL_BUF="${QSL_STRESS_SMALL_BUF:-2048}"
LARGE_BUF="${QSL_STRESS_LARGE_BUF:-8388608}"

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
    local pathspecs=(. ":(exclude)results/socket_stress_summary.txt"
        ":(exclude)results/socket_profile_loopback.txt")
    out_rel="$(repo_relative_or_empty "$OUT")"
    [[ -n "$out_rel" ]] && pathspecs+=(":(exclude)$out_rel")
    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-tree check failed; refusing to write misleading metadata." >&2
        exit 2
    fi
    if [[ -n "$status_output" ]]; then echo yes; else echo no; fi
}

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the dev preset first (make build)." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
DIRTY="$(dirty_tree_status)"

# Run TRIALS trials for one SO_RCVBUF request. Each trial: start the gap-tracking subscriber
# (background), let it bind, then burst-publish every datagram with no pacing. Emits one line:
#   "<label>|<requested>|<effective>|<published>|<gaps_csv>|<max_gaps>"
run_case() {
    local label="$1" req_buf="$2" port="$3"
    local effective="?" published="?" gaps_csv="" max_gaps=0
    local t sub_out pub_out gaps eff pub
    for ((t = 0; t < TRIALS; t++)); do
        sub_out="$(mktemp)"
        pub_out="$(mktemp)"
        "$BIN" subscribe "$port" 1000000 "$req_buf" >"$sub_out" 2>&1 &
        local sub_pid=$!
        sleep 0.5
        "$BIN" publish "$port" "$SEED" "$ORDERS" >"$pub_out" 2>&1 || true
        wait "$sub_pid" 2>/dev/null || true

        pub="$(grep -oE 'md_seq up to [0-9]+' "$pub_out" | grep -oE '[0-9]+' | tail -1 || true)"
        gaps="$(grep -oE 'total gaps detected: [0-9]+' "$sub_out" | grep -oE '[0-9]+' | tail -1 || true)"
        eff="$(grep -oE 'SO_RCVBUF=[0-9]+' "$sub_out" | grep -oE '[0-9]+' | tail -1 || true)"
        [[ -n "$pub" ]] && published="$pub"
        [[ -n "$eff" ]] && effective="$eff"
        gaps="${gaps:-?}"
        gaps_csv="${gaps_csv:+$gaps_csv,}$gaps"
        if [[ "$gaps" =~ ^[0-9]+$ ]] && ((gaps > max_gaps)); then max_gaps="$gaps"; fi

        rm -f "$sub_out" "$pub_out"
        port=$((port + 1))
    done
    printf '%s|%s|%s|%s|%s|%s\n' "$label" "$req_buf" "$effective" "$published" "$gaps_csv" "$max_gaps"
}

# Wide port spacing per setting so retries never collide across trials.
SMALL_LINE="$(run_case "small" "$SMALL_BUF" "$PORT_BASE")"
DEFAULT_LINE="$(run_case "default" 0 "$((PORT_BASE + 20))")"
LARGE_LINE="$(run_case "large" "$LARGE_BUF" "$((PORT_BASE + 40))")"

TMP_OUT="$(mktemp)"
{
    echo "Command:     bash scripts/socket_stress.sh  (make socket-stress)"
    echo "Artifact:    UDP socket-buffer / burst-loss experiment (loopback, constrained)"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    if [[ "$(uname -s)" == "Linux" ]]; then
        echo "CPU:          $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || true)"
        echo "rmem_default: $(cat /proc/sys/net/core/rmem_default 2>/dev/null || echo unknown)"
        echo "rmem_max:     $(cat /proc/sys/net/core/rmem_max 2>/dev/null || echo unknown)"
    fi
    echo "Compiler:    $(c++ --version | head -1)"
    echo "Git commit:  $(git rev-parse --short HEAD)"
    echo "Dirty tree:  $DIRTY"
    echo "Transport:   UDP unicast over 127.0.0.1 (loopback)"
    echo "Dataset:     qsl-mdfeed publish, seed $SEED, $ORDERS orders, 3 symbols"
    echo "Trials/setting: $TRIALS"
    echo "Date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Setup: one publisher bursts every market-data datagram back-to-back (no pacing); one"
    echo "subscriber drains them while detecting sequence gaps. The only variable is the"
    echo "subscriber's requested SO_RCVBUF. Requested 0 = OS default; the kernel may round up or"
    echo "clamp the request, so the effective (granted) size is read back via getsockopt."
    echo
    printf '%-9s %13s %13s %11s  %-20s %8s\n' \
        "setting" "requested(B)" "effective(B)" "published" "gaps/trial" "max"
    printf '%-9s %13s %13s %11s  %-20s %8s\n' \
        "-------" "------------" "------------" "---------" "----------" "---"
    for line in "$SMALL_LINE" "$DEFAULT_LINE" "$LARGE_LINE"; do
        IFS='|' read -r label req eff pub gaps_csv maxg <<<"$line"
        printf '%-9s %13s %13s %11s  %-20s %8s\n' "$label" "$req" "$eff" "$pub" "$gaps_csv" "$maxg"
    done
    echo
    echo "Reading the result: a too-small receive buffer overflows during the burst and the"
    echo "kernel silently drops datagrams, which the sequence tracker reports as gaps. The OS"
    echo "default and larger buffers absorb the same burst with little or no loss. Because UDP"
    echo "loss is timing/OS/load dependent, gap counts vary between trials and runs -- a small"
    echo "buffer does not lose on every trial. The mechanism (SO_RCVBUF bounds in-kernel"
    echo "queueing, so an undersized buffer drops under burst) is the point, not a fixed number."
    echo
    echo "Caveats:"
    echo "- Loopback only: no NIC, driver, routing, or real-network loss is exercised."
    echo "- This measures the receive socket-buffer / kernel-drop mechanism, not throughput,"
    echo "  not latency, and not a production capacity claim."
    echo "- No retransmit channel exists; loss is detected (gap count), never recovered."
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"

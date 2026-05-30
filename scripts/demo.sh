#!/usr/bin/env bash
# Local end-to-end demo of the exchange simulator. Two parts:
#   1. Deterministic event log -> replay/recovery (no network, no timing).
#   2. TCP order gateway round-trip over loopback.
#
# Security note: the gateway is UNAUTHENTICATED and binds 127.0.0.1 only. It is a local
# simulator for demonstration, not a real venue. Do not expose it on a public interface.
set -euo pipefail

cd "$(dirname "$0")/.."
PORT="${1:-9009}"
BIN="build/dev"

# Build the dev binaries (apps land at build/dev/qsl-*) if they are missing.
if [[ ! -x "$BIN/qsl-gateway" || ! -x "$BIN/qsl-replay" ]]; then
    make build >/dev/null
fi

LOG="$(mktemp "${TMPDIR:-/tmp}/qsl-demo.XXXXXX")"
GW_PID=""
cleanup() {
    [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null || true
    rm -f "$LOG"
}
trap cleanup EXIT

echo "== 1. Deterministic event log + replay/recovery (seed 42) =="
"$BIN/qsl-replay" generate "$LOG" 42
"$BIN/qsl-loginspect" "$LOG"
echo "-- rebuilding engine state from the log --"
"$BIN/qsl-replay" "$LOG"

echo
echo "== 2. TCP order gateway round-trip (127.0.0.1:$PORT, no auth, loopback) =="
"$BIN/qsl-gateway" "$PORT" &
GW_PID=$!
disown "$GW_PID" 2>/dev/null || true

ready=0
for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3>&- 3<&-
        ready=1
        break
    fi
    sleep 0.1
done
[[ "$ready" == 1 ]] || {
    echo "gateway did not become ready on port $PORT" >&2
    exit 1
}

"$BIN/qsl-client" "$PORT"

echo
echo "demo complete."

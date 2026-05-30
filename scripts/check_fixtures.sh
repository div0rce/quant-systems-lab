#!/usr/bin/env bash
# Golden check: regenerate the C++-produced differential fixtures and diff them against the
# committed copies. Fails if the C++ exporter output has drifted, so the cross-language
# differential tests can never silently compare OCaml against a stale C++ snapshot.
# (Hand-authored fixtures like stream_bad_*.txt are intentionally not regenerable.)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=build/dev/qsl-export-stream
[ -x "$BIN" ] || cmake --build --preset dev --target qsl-export-stream >/dev/null

F=ocaml/test/fixtures
status=0
check() { # $1 = committed file; $2.. = exporter args
    local file="$1"
    shift
    if ! "$BIN" "$@" | diff -q - "$file" >/dev/null; then
        echo "DRIFT: $file != current C++ output ($BIN $*)" >&2
        status=1
    fi
}

check "$F/stream_seed7.txt" 7 60
check "$F/stream_ioc.txt" ioc
for s in 1 2 3 4 5 6 7 8; do check "$F/prop_seed$s.txt" prop "$s"; done
check "$F/shrunk_seed1.txt" shrink 1

[ "$status" -eq 0 ] && echo "fixtures match current C++ output"
exit "$status"

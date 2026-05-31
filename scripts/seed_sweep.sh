#!/usr/bin/env bash
# CI seed sweep (issue #35): generate property fixtures for seeds 1..N with the C++ exporter and
# check each against the independent OCaml replay engine (differential), going beyond the 8
# committed property fixtures. Exits non-zero on any C++/OCaml divergence; per-fixture bundles
# are written to ocaml/_diff for CI upload.
set -euo pipefail
cd "$(dirname "$0")/.."

N="${1:-64}"
BIN=build/dev/qsl-export-stream
[ -x "$BIN" ] || cmake --build --preset dev --target qsl-export-stream >/dev/null

SWEEP="$(mktemp -d)"
trap 'rm -rf "$SWEEP"' EXIT
for s in $(seq 1 "$N"); do
    "$BIN" prop "$s" >"$SWEEP/prop_seed$s.txt"
done

mkdir -p ocaml/_diff
(cd ocaml && dune exec bin/diff_report.exe -- "$PWD/_diff" "$SWEEP"/prop_seed*.txt)
echo "seed sweep: seeds 1..$N differential OK"

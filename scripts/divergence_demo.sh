#!/usr/bin/env bash
# Divergence demonstration (issue #37): the shrinker reducing a *real* C++ vs OCaml mismatch,
# not just the artificial "produces a trade" predicate.
#
# We inject a bug via the OCaml oracle's `--drop-cancels` mode, have the C++ exporter shrink a
# seeded property flow down to a minimal counterexample (`qsl-export-stream divergence`), then
# show that on that minimal stream the honest OCaml replay AGREES with the embedded C++ snapshot
# while the buggy oracle DIVERGES. Self-verifying: exits non-zero if the expected agree/diverge
# relationship does not hold.
set -euo pipefail
cd "$(dirname "$0")/.."

SEED="${1:-1}"

EXPORT=build/dev/qsl-export-stream
[ -x "$EXPORT" ] || cmake --build --preset dev --target qsl-export-stream >/dev/null
(cd ocaml && dune build)
OCAML=ocaml/_build/default/bin/replay_snapshot.exe

TMP="$(mktemp)"
trap 'rm -f "$TMP" "$TMP.emb" "$TMP.real" "$TMP.mut"' EXIT

"$EXPORT" divergence "$SEED" >"$TMP"
grep -E '^(snapshot|sym|level) ' "$TMP" >"$TMP.emb" # embedded (correct) C++ snapshot
"$OCAML" "$TMP" >"$TMP.real"
"$OCAML" --drop-cancels "$TMP" >"$TMP.mut"

echo "=== minimal divergence counterexample (shrinker output) ==="
grep -vE '^evt ' "$TMP"

echo "=== honest OCaml replay vs C++ snapshot ==="
if diff -q "$TMP.emb" "$TMP.real" >/dev/null; then
    echo "AGREE, no real divergence between C++ and OCaml"
else
    echo "UNEXPECTED: honest OCaml replay disagrees with C++" >&2
    exit 1
fi

echo "=== buggy OCaml (--drop-cancels) vs C++ snapshot ==="
if diff -q "$TMP.emb" "$TMP.mut" >/dev/null; then
    echo "UNEXPECTED: injected bug produced no divergence" >&2
    exit 1
fi
echo "DIVERGE, shrinker reproduced the injected C++/OCaml mismatch at minimal size:"
diff "$TMP.emb" "$TMP.mut" || true

echo "divergence demo OK"

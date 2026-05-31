# Differential regression archive

A folder of **minimal reproducers** for differential-testing failures worth keeping over time.
When the cross-language differential (C++ engine vs the independent OCaml replay) ever surfaces a
real divergence, the shrinker reduces it to a small command stream; that minimized fixture is
archived here with a note on the root cause, so the exact failure is preserved even if the
generator or shrinker later changes (these entries are intentionally **not** golden-regenerated).

## Status

The C++ engine and the OCaml oracle currently agree on every tested stream — **no real bug has
been found.** This archive is therefore seeded with one *synthetic* entry so the structure and
workflow exist for when a real divergence appears.

## Entries

| File | Kind | Summary |
| --- | --- | --- |
| `0001-synthetic-cancel-dropping-divergence.txt` | synthetic (injected) | The issue #37 demonstration: a 123-command flow shrunk to 3 commands (`reg; bid limit; cancel`) on which a deliberately buggy oracle that drops cancels diverges from the correct engine. |

## Format

Each entry is a minimized command-stream fixture (same schema as
`ocaml/test/fixtures/*.txt`, see `docs/differential_testing.md`) with a header comment block
recording the seed, original/minimized lengths, and the failure (root cause). The embedded
`snapshot` lines are the **correct** engine result.

## Reproducing an entry

```bash
cmake --build --preset dev --target qsl-export-stream
(cd ocaml && dune build)
OCAML=ocaml/_build/default/bin/replay_snapshot.exe

# The honest OCaml replay matches the embedded (correct) C++ snapshot:
"$OCAML" regressions/0001-synthetic-cancel-dropping-divergence.txt

# Entry 0001 only diverges under the injected bug (the cancel-dropping oracle):
"$OCAML" --drop-cancels regressions/0001-synthetic-cancel-dropping-divergence.txt
```

## Adding a real entry

1. Capture the failing seed and shrink it (`qsl-export-stream` / `replay::shrink`).
2. Save the minimized fixture as `NNNN-short-slug.txt`.
3. Add a row above with the root cause, and link the fixing PR/commit.

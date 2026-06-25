# ADR 0003: Golden fixture regeneration and provenance manifest

## Status

Accepted

## Context

The differential fixtures embed a C++ snapshot that the OCaml oracle is compared against. If a
committed fixture drifted from current C++ output, the differential test could silently compare
OCaml against a stale snapshot. We also wanted reproducible, tamper-evident provenance for the
committed corpus.

## Decision

- Commit the generator-produced fixtures (`stream_seed7`, `stream_ioc`, `shrunk_seed1`,
  `prop_seed1..50`) and **regenerate-and-diff** them in CI (`make check-fixtures`,
  `scripts/check_fixtures.sh`). Hand-authored negatives (`stream_bad_*`) are intentionally not
  regenerated.
- Record a provenance manifest (`MANIFEST.txt` via `make check-manifest`) with the generator
  version (`replay::kGeneratorVersion`), the exporter invocation per fixture, and a SHA-256.
- Keep generation integer-only and `mt19937_64`-based, and assert byte-identical output across
  compilers (`make determinism`).

## Consequences

- A fixture cannot drift from current C++ output, or change without a visible manifest update.
- The corpus is reproducible across compilers/platforms (macOS-committed vs Linux CI).
- The generator version is human-set provenance, not an automatically enforced invariant
  (a generator change still requires a deliberate version bump, documented, not enforced).

## Alternatives considered

- **Trust committed fixtures without regeneration**, rejected: silent drift risk.
- **Hashes only (no regeneration)**, rejected: a hash matches a stale-but-unchanged fixture; the
  regenerate-and-diff step is what proves equivalence to current C++ output.

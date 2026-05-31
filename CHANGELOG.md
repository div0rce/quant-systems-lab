# Changelog

All notable changes to this project. The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/); this project has not yet cut a tagged release.

## [Unreleased]

### Post-M22 backlog hardening (GitHub issues)

- Dedicated differential negative fixtures for `best_bid`/`best_ask`/trade-count/bid-side levels (#36).
- Differential oracle self-test: an injected divergence is detected and shrunk to a minimal reproducer (#34).

### Phase II — cross-language differential testing (M15–M20)

- Normalized command-stream + final-snapshot fixture export (M15).
- Independent OCaml replay engine that recomputes the final snapshot from the command stream
  alone (M16).
- Differential tests asserting C++ and OCaml snapshots are equal, with a deliberate-mismatch
  fixture and a golden fixture-regeneration guard in CI (M17).
- Seeded property-based command generator spanning valid/invalid/duplicate/reused/unknown/IOC/
  market/cancel/modify/multi-symbol cases (M18).
- Deterministic shrinker that reduces a failing stream to a minimal counterexample, with a
  minimal-fixture exporter (M19).
- Differential-testing and property-testing architecture documentation (M20).

### Core simulator and tooling (M3–M14)

- OCaml replay verifier checking exported event logs against replay invariants (M14).
- Final architecture/demo/recruiting documentation and `make demo` (M13).
- Hardening: ASan/UBSan, randomized invariant tests, and structure-aware protocol fuzzing in CI
  (M12).
- Reproducible benchmark harness writing `results/latest.txt` with full metadata (M11).
- Append-only event log and deterministic replay/recovery (M7–M8).
- Price-time-priority matching engine, deterministic risk checks, and a market-data publisher
  (M3–M6).

(Networking: a loopback TCP order gateway (M9) and UDP market-data feed (M10) — local,
unauthenticated; see `SECURITY.md`.)

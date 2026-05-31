# Changelog

All notable changes to this project. The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/); this project has not yet cut a tagged release.

## [Unreleased]

### Post-M22 backlog hardening (GitHub issues #34–#51)

Differential/property-testing follow-ups, each merged as an individual Codex-reviewed PR:

- Oracle self-test that injects a divergence and shrinks it to a minimal reproducer (#34).
- Dynamic CI seed sweep beyond the committed fixtures (#35).
- Dedicated negative fixtures for `best_bid`/`best_ask`/trade-count/bid-side levels (#36).
- Synthetic divergence demonstration: the shrinker reducing a real C++/OCaml mismatch (#37).
- Differential fixture coverage matrix (#38) and an oracle-independence audit (#39).
- CI failure artifact bundle uploaded on divergence (#40).
- Shared gateway-dispatch helper (#41); price (#42) and symbol/order-id (#43) shrink passes.
- Generator reject-reason coverage test (#44); cross-compiler determinism check (#45).
- Shrinker effectiveness metrics (#46) and a seed-reproducibility manifest (#47).
- Oracle mutation testing across every snapshot field (#48).
- Larger committed property corpus, `prop_seed1..50` (#49).
- Differential regression archive (#50).
- Differential-harness performance benchmarks, `results/differential.txt` (#51).

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

# Release Readiness Audit (M22)

A final pre-release pass (the Phase II equivalent of M13): verify the repo builds, demos,
reproduces, and reads honestly from M0 through M21. No GitHub release is created here (that is
the optional, human-approved M23).

## Verification (this session, arm64 / Apple clang 17)

| Check | Result |
|---|---|
| `make check` | 152/152 tests pass, no warnings |
| `make asan` (ASan + UBSan) | 152/152, sanitizer-clean |
| `make check-fixtures` | committed fixtures match current C++ output |
| `dune runtest --root ocaml` | verifier + independent replay + differential all pass |
| `make demo` | clean, deterministic (replay/recovery + loopback gateway round-trip) |
| `make bench` | reruns and reproduces; committed `results/latest.txt` retained (single-machine, run-to-run variance — not overwritten) |

## Documentation

- **Links resolve.** All README links (15) and all doc-to-doc links resolve.
- **No overclaiming.** A scan for forbidden phrases (production-grade, formal verification, HFT
  platform, low-latency trading, real exchange, trading bot, production exchange) found only
  negations ("not formal verification", "not production exchange throughput", "not a real
  exchange") and the project's own avoid-lists/specs — no positive claims.
- **Benchmark language** remains measured, synthetic, hardware-dependent, and reproducible from
  the committed harness; numbers are cited from `results/latest.txt` only.
- **Differential-testing vocabulary is distinct** across the docs: log-invariant checking
  (`docs/ocaml_verifier.md`), independent OCaml replay (M16), C++-vs-OCaml differential snapshot
  comparison (M17), property-based generation (M18), and shrinking to minimal fixtures (M19) —
  see `docs/differential_testing.md` and `docs/property_testing.md`.
- **No stale milestone references**: PROGRESS and the milestone tables reflect M0–M21 merged and
  M22 in progress.

## Scope and honesty

This is a deterministic exchange-systems lab / research portfolio project — not a production
exchange, not connected to real markets, and making no latency or profitability claims. The
demo network services are unauthenticated and loopback-only (`SECURITY.md`).

## Outcome

Release-ready as a portfolio artifact. An optional conservative GitHub-only `v0.1.0` release is
deferred to M23 and requires explicit human approval.

# Release Readiness Audit

A pre-release pass verifying the repo builds, demos, reproduces, and reads honestly across
**M0–M22 and the post-M22 backlog (#34–#51)**. No GitHub release is created here (that is the
optional, human-approved M23). This audit was re-run after the backlog landed, so it reflects
the current state, not the original M22 snapshot.

## Verification (this session, arm64 / Apple clang 17)

| Check | Result |
|---|---|
| `make check` | 157/157 tests pass, no warnings |
| `make asan` (ASan + UBSan) | 157/157, sanitizer-clean |
| `make check-fixtures` | committed differential fixtures match current C++ output |
| `make check-manifest` | provenance manifest matches the committed fixtures |
| `make determinism` | every generated fixture (incl. all 50 property seeds) is byte-identical across gcc/clang and vs the committed macOS/AppleClang copies |
| `make divergence-demo` | the shrinker reduces an injected C++/OCaml divergence to a 3-command counterexample; the honest OCaml replay agrees, the `--drop-cancels` oracle diverges |
| `dune runtest --root ocaml` | 5 suites pass: log-invariant verifier, independent replay engine, differential replay (50 property fixtures), failure-bundle (`diff_report`), and oracle mutation testing |
| `make demo` | clean, deterministic (replay/recovery + loopback gateway round-trip) |
| `make bench` / `make bench-diff` | reproduce from the committed harness; `results/latest.txt` and `results/differential.txt` are retained (single-machine, run-to-run variance — not overwritten here) |

CI mirrors these across five jobs: `build-test` (build + test + bench compile-check +
`check-fixtures` + `check-manifest` + fmt), `sanitizers`, `ocaml-verifier` (with a failure
artifact bundle uploaded on divergence), `differential-sweep` (seeds 1..64 + the divergence
demo), and `determinism` (gcc + clang).

## Documentation

- **Links resolve.** README and doc-to-doc links resolve; the architecture and differential
  diagrams render (README compact; `docs/architecture.md` fuller; specialized diagrams in
  `docs/differential_testing.md`, `docs/property_testing.md`, `docs/replay_and_recovery.md`,
  `docs/benchmarking.md`).
- **No overclaiming.** A scan for forbidden phrases (production-grade, formal verification, HFT
  platform, low-latency trading, real exchange, trading bot, production exchange) finds only
  negations and the project's own avoid-lists/specs — no positive claims.
- **Benchmark language** remains measured, synthetic, hardware-dependent, and reproducible from
  the committed harness; core numbers are cited from `results/latest.txt` and the
  differential-harness numbers from `results/differential.txt` only.
- **Differential-testing vocabulary is distinct and current**: log-invariant checking
  (`docs/ocaml_verifier.md`), independent OCaml replay (M16), C++-vs-OCaml differential snapshot
  comparison (M17), property generation (M18), shrinking to minimal fixtures (M19), plus the
  backlog hardening — oracle self-test (#34), CI seed sweep (#35), negative fixtures (#36),
  synthetic divergence demo (#37), coverage matrix (#38), oracle-independence audit (#39),
  failure artifact bundle (#40), price/symbol-id shrink passes (#42, #43), reject-reason coverage
  (#44), cross-compiler determinism (#45), shrinker metrics (#46), reproducibility manifest
  (#47), oracle mutation testing (#48), 50-seed corpus (#49), regression archive (#50), and
  differential-harness benchmarks (#51).
- **Architecture decisions** are recorded in `docs/adr/` (independent OCaml oracle, golden
  fixture regeneration, deterministic shrinker, dynamic concurrency validation limits,
  allocator-vs-storage separation, constrained perf artifacts).
- **No stale milestone references**: PROGRESS and the milestone tables reflect the current merged
  milestones and clearly distinguish still-open backlog from follow-ups already addressed by later
  branches.

## Scope and honesty

This is a deterministic exchange-systems lab / research portfolio project — not a production
exchange, not connected to real markets, and making no latency or profitability claims. The
demo network services are unauthenticated and loopback-only (`SECURITY.md`). The cross-language
differential layer is property-based testing against the C++ system under test, **not** formal
verification.

## Outcome

Release-ready as a portfolio artifact. An optional, conservative GitHub-only `v0.1.0` release is
deferred to M23 and requires explicit human approval.

## Post-Release Roadmap Note

After M28/M29 review, the roadmap distinguishes workflow validation from final evidence:

- M29 lands Linux `perf` tooling, metadata-rich artifacts, dirty-tree handling, PMU validation,
  constrained-environment validation, and CI validation.
- The current committed M29 artifacts were generated in a constrained Docker Desktop Linux
  environment and are not real hardware PMU evidence.
- Issue #90 tracks full PMU-backed artifacts on a bare-metal or PMU-capable Linux target.
- TSan coverage is dynamic-analysis evidence over executed schedules, not proof over all possible
  interleavings.
- M28 allocator evidence did not change order-book storage architecture; pool-backed order-book
  integration is a separate roadmap item.

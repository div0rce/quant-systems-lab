# Release Readiness Audit

A pre-release pass verifying the repo builds, demos, reproduces, and reads honestly. This audit
covers **M0-M49, the v0.2.0 evidence refresh** (bare-metal Linux artifact regeneration and the
documentation/staleness sweep), **the v0.2.1 content** (the FIX-like text protocol adapter #29, the
perf call-graph flamegraph + `make flamegraph` #32, and a Codex resume-anchor/PMU consistency sweep),
**and the post-v0.2.1 hardening + perf wave being cut as v0.2.2** (#135, #146): out-of-domain enum
rejection in the decoders (#136), network-path hardening. EINTR retry, accept fairness, connection
cap, UDP send-error tracking, transient-accept survival, and fd-exhaustion handling (#137/#140/#143),
CLI argument validation (#141), a real UBSan abort gate (#142), OCaml `diff_report` robustness (#144),
and two measured order-book perf wins, `try_emplace` (~+5%, #138) and an index load-factor cap
(~+18.6%, #145). It supersedes the v0.1.0-era audit; the actual GitHub release is cut by a human
after squash-merge.

## Verification (this session, bare-metal Apple M2 / aarch64 / GCC 16.1.1, Fedora Asahi Remix)

| Check | Result |
|---|---|
| `make check` | 270/270 tests pass, no warnings (incl. the FIX-adapter, flamegraph-renderer, decoder enum-rejection, and CLI-arg-validation tests) |
| `make asan` (ASan + UBSan) | 270/270, sanitizer-clean; the UBSan gate now **aborts** on the first violation (`-fno-sanitize-recover=undefined`, #142), so pure-UBSan defects no longer pass green, and the tree is clean under it |
| `make tsan` (ThreadSanitizer) | 20/20 concurrency-labelled tests, race-clean |
| `make check-fixtures` | committed differential fixtures match current C++ output |
| `make check-manifest` | provenance manifest matches the committed fixtures |
| `make determinism` | every generated fixture (incl. all 50 property seeds) is byte-identical across gcc/clang and vs the committed copies |
| `make divergence-demo` | the shrinker reduces an injected C++/OCaml divergence to a 3-command counterexample; the honest OCaml replay agrees, the `--drop-cancels` oracle diverges |
| `dune runtest --root ocaml` | suites pass: log-invariant verifier, independent replay engine, differential replay (50 property fixtures), failure-bundle (`diff_report`), and oracle mutation testing |
| `make demo` | clean, deterministic (replay/recovery + loopback gateway round-trip) |
| `make bench` / `make bench-diff` | reproduce from the committed harness; `results/latest.txt` and `results/differential.txt` are bare-metal Apple M2 runs (single-machine, run-to-run variance) |
| `make flamegraph` (Linux perf, v0.2.1) | bare-metal cpu-clock call-graph profile → `results/flamegraph.svg`/`.txt`, classified `software cpu-clock sampling hot-symbol profile`, gated on the folded sample total, `Dirty inputs: no` |

CI mirrors these across six jobs: `build-test` (build + test + bench compile-check +
`check-fixtures` + `check-manifest` + fmt), `sanitizers` (ASan/UBSan), `thread-sanitizer`,
`ocaml-verifier` (with a failure artifact bundle uploaded on divergence), `differential-sweep`
(seeds 1..64 + the divergence demo), and `determinism` (gcc + clang). External checks add CodeScene
Code Health and CodeRabbit review.

## Evidence environment (v0.2.0)

The committed `results/*.txt` artifacts are now generated on a **bare-metal** Apple MacBook Air
(M2, aarch64) running Fedora Asahi Remix, not the earlier Docker Desktop Linux. What that does and
does not buy:

- **Perf**, `results/perf_stat_linux.txt` is **partial hardware PMU evidence**: real `cycles` /
  `instructions` / `branches` / `branch-misses` off the Apple Avalanche/Blizzard PMUs, with
  `cache-references` / `cache-misses` reported `<not supported>` (Apple Silicon PMU limitation).
  Not full PMU evidence; issue #90 tracks the cache-counter set, which needs a different PMU.
  `results/flamegraph.svg`/`.txt` (v0.2.1) is a **software cpu-clock sampling** hot-symbol profile
  from the same host, a hot-symbol investigation aid, not a latency or throughput claim.
- **Sockets**, `socket_profile_loopback.txt`, `socket_stress_summary.txt`, and
  `socket_load_summary.txt` are bare-metal but **loopback-only**: no NIC/driver/routing.
- **NUMA**, `numa_affinity_study.txt` is bare-metal but the M2 is a **single-NUMA-node** machine,
  so it is `linux-constrained` for NUMA (real CPU pinning, no cross-node binding to measure).
- **Benchmarks**, `latest.txt`, `pool_backed_storage.txt`, `recovery_benchmarks.txt`,
  `allocator_experiment.txt`, `false_sharing_study.txt`, `differential.txt` are bare-metal but
  **synthetic, single-process microbenchmarks**.

Every artifact carries source-digest provenance and reports `Dirty inputs: no`; no committed
artifact leaks host identifiers (a publish-time MAC sanitizer redacts every non-broadcast MAC).

## Documentation

- **Links resolve.** README and doc-to-doc links resolve; the architecture and differential
  diagrams render (README compact; `docs/architecture.md` fuller; specialized diagrams in
  `docs/differential_testing.md`, `docs/property_testing.md`, `docs/replay_and_recovery.md`,
  `docs/benchmarking.md`).
- **No overclaiming.** A scan for forbidden phrases (production-grade, formal verification, HFT
  platform, low-latency trading, real exchange, trading bot, production exchange) finds only
  negations and the project's own avoid-lists/specs, no positive claims.
- **Benchmark language** remains measured, synthetic, hardware-dependent, and reproducible from the
  committed harness; core numbers are cited from `results/latest.txt` and the differential-harness
  numbers from `results/differential.txt` only.
- **Architecture decisions** are recorded in `docs/adr/` (independent OCaml oracle, golden fixture
  regeneration, deterministic shrinker, dynamic concurrency validation limits, allocator-vs-storage
  separation, constrained/partial perf artifacts, loopback socket evidence, PMR node allocation,
  epoll prototype, durability modes and tail repair).
- **No stale milestone references**: PROGRESS, HANDOFF, and the milestone tables reflect the merged
  M0-M49 state, the v0.2.0 artifact refresh, and the v0.2.1 content (#29/#32 closed; resume anchors
  consistent across PROGRESS/HANDOFF/AGENTS/CLAUDE).

## Scope and honesty

This is a deterministic exchange-systems lab / research portfolio project, not a production
exchange, not connected to real markets, and making no latency or profitability claims. The demo
network services are unauthenticated and loopback-only (`SECURITY.md`). The cross-language
differential layer is property-based testing against the C++ system under test, **not** formal
verification.

## Standing credibility gaps (open, not blockers)

- **Issue #94**, no independent external technical review yet. The repo is self-certified.
- **Issue #90**, full cache-counter PMU evidence still absent; the bare-metal Apple PMU provides a
  partial counter set only.

## Outcome

Release-ready as a portfolio artifact. `v0.2.2` is tagged on top of `v0.2.1` (FIX adapter #29, perf
flamegraph issue #32, anchor sweep) and `v0.2.0` (Phase III/IV systems work, M24-M49, plus the
bare-metal evidence refresh). `v0.2.2` bundled the post-v0.2.1 hardening + perf wave (#135, #146:
decoder enum rejection, network/CLI hardening, a real UBSan abort gate, OCaml diff_report robustness,
and the two measured order-book perf wins) plus a full documentation overhaul (#147, #149), a
reproducible performance-evidence report comparing v0.1.0 to v0.2.2 (#148), and a bug/style sweep
with mermaid diagrams (#150). Each release is a GitHub-only tag with explicit human approval.

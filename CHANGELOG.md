# Changelog

All notable changes to this project. The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/). The first tagged release is v0.1.0.

## [Unreleased]

### Added

- M32: `OrderBook::Storage::{Baseline,Pooled}` and `MatchingEngine(OrderBook::Storage)` for a
  scoped PMR-backed order-book node-allocation experiment. `Storage::Pooled` routes per-book
  `std::list`, `std::map`, and `std::unordered_map` node allocation through
  `std::pmr::unsynchronized_pool_resource`; baseline storage remains the default.
- M32: `make bench-storage` / `scripts/run_storage_benchmarks.sh` / `qsl-bench storage`, an
  engine-level benchmark comparing baseline order-book storage against PMR pooled node allocation.
- M32: issue #95 tracks the separate future intrusive/custom-node `OrderPool<Capacity>`
  integration path.
- M30: optional UDP receive-buffer (`SO_RCVBUF`) sizing on the market-data client, with the
  granted size read back via `getsockopt`. `qsl-mdfeed subscribe` gains a `[rcvbuf_bytes]`
  argument and `qsl-mdfeed publish` an `[orders]` burst-size argument.
- M30: `scripts/profile_gateway_io.sh` (`make profile-io`, Linux-only) — profiles the gateway
  syscall / kernel-socket path with `strace -f -c` plus procfs rusage (`/proc/<pid>/{stat,status}`),
  distinguishing user-space matching cost from kernel/socket overhead.
- M30: `scripts/socket_stress.sh` (`make socket-stress`, portable) — UDP burst/gap and
  receive-socket-buffer experiment over loopback, run over multiple trials.

### Documentation

- M32: added `docs/pool_backed_storage.md` and ADR 0009 to distinguish M28 raw-object pool
  mechanics, M32 PMR container-node allocation, and the future intrusive/custom-node order-book
  redesign. Added `results/pool_backed_storage.txt` as the measured engine-level artifact.
- M31: added an external-review package — `docs/review_request.md` (an adversarial review
  checklist over SPSC memory ordering, backpressure, threaded ownership, event-log integrity under
  concurrency, and benchmark/profiling + Linux/socket methodology), `docs/review_feedback.md` (an
  honest, auditable feedback record stating no external review has occurred yet — no fabricated
  endorsements), and a `.github/ISSUE_TEMPLATE/review_request.md`. README now states claims are
  self-certified and links the request. Distinguishes self-certified vs externally-reviewed claims.
- M30: added `docs/socket_profiling.md` (syscall/rusage + UDP-loss methodology and how to read
  the artifacts) and `docs/socket_hardening.md` (socket defensive posture, the `SO_RCVBUF` knob,
  and what is intentionally out of scope). Recorded ADR 0008 (socket evidence is
  loopback-constrained; `epoll` deferred to M34/M35). Committed constrained loopback artifacts
  `results/socket_profile_loopback.txt` and `results/socket_stress_summary.txt`.
- Classified PR #89 / M29 as Linux `perf` workflow plus constrained-environment validation, not
  full hardware PMU evidence.
- Recorded issue #90 as the follow-up for full Linux hardware PMU perf artifacts on a PMU-capable
  Linux target.
- Expanded the post-M29 roadmap through M41, covering pool-backed order-book storage, advanced
  concurrency validation, epoll gateway work, multi-client socket pressure, NUMA studies,
  lock-free ingress, stronger persistence, recovery benchmarking, DPDK research, and NIC offload
  study.
- Added explicit backlog distinctions: TSan coverage is dynamic-analysis evidence rather than
  proof, and M28 allocator results are allocator evidence rather than engine-storage evidence.

## [0.1.0] - 2026-05-31

Quant Systems Lab v0.1.0 — a deterministic C++20 exchange simulator and cross-language
differential-testing harness, built as a systems-engineering portfolio project. It is **not** a
production exchange, is not connected to real markets, and makes no latency or profitability
claims; the cross-language differential layer is property-based testing, **not** formal
verification. Benchmarks are synthetic microbenchmarks recorded in `results/` and are
hardware/compiler/build-dependent.

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

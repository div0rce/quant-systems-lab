# Recruiting Notes

> Internal notes on how to position this project. Conservative and technically defensible by
> design — every claim maps to code or a measured result. No fake metrics, no overclaiming.

## What this project demonstrates

1. Deterministic state-machine design (reproducible from an append-only log)
2. Integer price modeling (no floating-point prices)
3. Binary protocol with explicit serialization and malformed-input handling
4. Replay/recovery from append-only logs
5. Invariant + fuzz testing beyond happy-path demos, under ASan/UBSan
6. Reproducible benchmark methodology
7. Clean incremental engineering process (one milestone = one reviewed PR)

## What this project is NOT

- Not a production exchange, trading bot, or connected to real markets
- Not making profitability, "production-grade", or "battle-tested" claims

## Résumé bullets — Software Engineering (conservative)

- Built a deterministic C++20 multi-symbol matching engine (price-time priority, partial
  fills, cancel/modify) emitting a strictly-increasing event stream with integer-tick prices.
- Designed a fixed-width big-endian binary protocol with explicit byte (de)serialization (no
  `reinterpret_cast` of structs) and deterministic rejection of malformed/truncated frames.
- Implemented append-only event logging with replay/recovery that rebuilds byte-identical
  engine state, verified by snapshot equality over randomized deterministic flows.
- Wrote property/invariant tests (no crossed book, executed ≤ submitted, strict sequencing)
  and structure-aware protocol fuzzing, run under AddressSanitizer/UBSan in CI.
- Set up the full toolchain: CMake/Ninja, clang-format/clang-tidy, GitHub Actions with a
  sanitizer job, and a reproducible benchmark harness committing results with metadata.
- Wrote an independent replay-invariant verifier in OCaml (typed, immutable) that re-checks
  exported C++ event-log fixtures against replay invariants — a cross-language check, not a
  re-implementation of the engine.
- Built a cross-language differential testing system: an independent OCaml engine replays
  seeded, property-generated command streams and its final snapshot is asserted equal to the
  C++ engine's (best bid/ask, level aggregates, order counts, sequence, trade count) in CI.
- Added a deterministic shrinker that reduces a failing command stream to a minimal
  counterexample, plus golden-regenerated fixtures so the comparison cannot drift from current
  C++ output.

## Résumé bullets — Linux Engineering (conservative)

- Implemented a single-threaded TCP order gateway and a UDP market-data feed on POSIX sockets
  (loopback), with bounded receive timeouts, sequence-gap detection, and
  disconnect-on-malformed-framing.
- Built CLI tools for append-only-log inspection and deterministic replay, plus a demo script
  that orchestrates a loopback gateway round-trip with port-readiness polling and clean
  process teardown.
- Hardened with ASan/UBSan, fixed RNG seeds for reproducibility, and a benchmark harness that
  records hardware/OS/compiler/build/commit metadata alongside results.

## Benchmark bullets (measured — cite with the caveat)

Single-machine synthetic, in-process microbenchmark (arm64, Apple clang 17, Release, seed 42;
from `results/latest.txt`). **Excludes** network I/O, disk fsync, the kernel/socket path, and
allocator tuning — not production throughput or end-to-end latency:

- matching-engine flow ~121 ns/command (~8.2M commands/sec)
- order-book add/modify/cancel ~126 ns/op
- protocol `NewOrder` encode+decode ~39 ns/op
- gateway session crossing-fill round-trip ~270 ns/op
- replay from command log ~132 ns/command

## Interview-defense notes

- **Why integer-tick prices?** Exact arithmetic and reproducible price-time priority;
  floating-point rounding would make fills and ordering non-deterministic.
- **Why no wall-clock in the core?** Determinism: ordering uses a logical `SeqNo`/`Timestamp`,
  so replay reproduces state exactly. `std::chrono` appears only in the benchmark layer.
- **How do you know replay is correct?** Snapshot equality (best bid/ask, resting-order
  counts, per-level aggregate quantities, trade sequence, last sequence) over randomized
  seeds, plus a stress test (multiple seeds × 8000 orders).
- **What about malformed input?** Decoders are bounds-safe and non-throwing and reject
  deterministically; structure-aware and mutated-valid-frame fuzzing runs under ASan/UBSan,
  and the session disconnects rather than risking stream desync. See `docs/invariants.md`.
- **Are the benchmarks meaningful?** As regression/order-of-magnitude signals, yes; they are
  explicitly microbenchmarks and I can enumerate what they exclude. I will not present them as
  production numbers.
- **Biggest weaknesses?** Single-threaded, synthetic, minimal networking, no real venue or
  persistence beyond the flat log — see the README Limitations section.
- **What would you do next?** Lock-free queues / memory-pool internals, a multithreaded
  pipeline with ThreadSanitizer, and a richer order-flow model (the cross-language differential
  testing system and OCaml oracle are already built — see the differential-testing docs).
- **Why OCaml, and what does it actually prove?** It's an independent cross-check: a small
  typed/immutable verifier parses the exported event log and re-derives replay invariants, so
  a bug in a shared C++ assumption is less likely to pass unnoticed. It does not re-implement
  matching and is explicitly not formal verification — I'd describe it as a disciplined
  external checker, not OCaml expertise theater.

# External review request

This is an **open request for technical criticism** of Quant Systems Lab. As of this writing
**no external review has taken place** — every claim in this repository is **self-certified** by
the maintainer (see [Review status](#review-status)). This document exists to make the project
easy to review well, and to invite reviewers to attack the parts most likely to be wrong.

If you review any part of this, please file an issue (the
[review-request issue template](../.github/ISSUE_TEMPLATE/review_request.md) gives a starting
structure) or open a PR. Accepted and rejected feedback is recorded honestly in
[`review_feedback.md`](review_feedback.md).

## What this project is (and is not)

A deterministic C++20 exchange **simulator** plus a cross-language differential-testing harness,
built as a systems-engineering portfolio project. It is **not** a production exchange, is not
connected to real markets, and makes no latency, throughput, or profitability claims. Benchmarks
are synthetic microbenchmarks; profiling artifacts are loopback, constrained-environment evidence.
See the [README](../README.md), [architecture](architecture.md), and [invariants](invariants.md)
for the full picture and the honest limitations.

## Where criticism is most valuable

Each area below states the claim, points to where it lives, and lists the specific questions a
reviewer is best placed to answer. Please be adversarial: the goal is to find what is wrong, not
to confirm what looks right.

### 1. SPSC ring-buffer memory ordering

- **Claim:** the bounded single-producer/single-consumer queue is correct with acquire/release
  ordering and is wait-free per operation (bounded steps, no CAS, no locks).
- **Where:** [`memory_ordering.md`](memory_ordering.md) (ordering table + happens-before
  argument), `include/qsl/concurrency/spsc_ring.hpp`, `tests/concurrency/test_spsc_stress.cpp`.
- **Questions:** Is the happens-before reasoning sound in both directions? Is any `acquire`/
  `release` actually too weak (or unnecessarily strong)? Are the index/wraparound and full/empty
  distinctions free of ABA or torn-read hazards for the supported payload types? Is the
  "wait-free per operation" claim correctly scoped (it excludes a caller spinning on backpressure)?

### 2. Backpressure semantics

- **Claim:** bounded-queue backpressure is **lossless** — under a full queue the producer spins/
  yields rather than dropping, and capacity changes affect only timing, never the result.
- **Where:** [`concurrency_model.md`](concurrency_model.md),
  `tests/concurrency/test_backpressure.cpp`, the gated-consumer/probe test in the pipeline suite.
- **Questions:** Can any interleaving drop or duplicate an item under backpressure? Is the
  deterministic-barrier test actually deterministic, or does it have a residual timing assumption?
  Is "capacity changes only affect backpressure, never the result" truly established by the
  capacity sweep (2..4096), or is there a gap?

### 3. Threaded ownership model

- **Claim:** the engine thread is the **sole owner** of the matching engine + gateway; cross-thread
  communication is a value hand-off through bounded SPSC queues, so deterministic matching is
  unchanged by concurrency.
- **Where:** [`concurrency_model.md`](concurrency_model.md),
  `include/qsl/concurrency/pipeline.hpp`, `tests/concurrency/test_pipeline.cpp`.
- **Questions:** Is there any shared mutable state crossing a thread boundary that the model
  misses? Does the downstream sink truly avoid reading engine state (it consumes self-contained
  records)? Are the SPSC lifetime brackets (rings on the run stack, joined before return) actually
  safe against use-after-scope on early shutdown?

### 4. Event-log integrity under concurrency

- **Claim:** accepted commands are never silently dropped from the event log under the threaded
  pipeline; shutdown drains pending work; a replay of the concurrently-written log reproduces the
  engine's final state.
- **Where:** [`concurrency_model.md`](concurrency_model.md),
  `tests/concurrency/test_pipeline.cpp` (event-log integrity + shutdown variants),
  [`replay_and_recovery.md`](replay_and_recovery.md).
- **Questions:** Under `shutdown_with_full_queue` / `shutdown_with_pending_commands`, can a record
  be lost or written out of order? Is the drain-then-stop protocol correct, or can the log-writer
  thread exit with records still in flight? Does the concurrent-write-then-replay equivalence hold
  across seeds, or is it under-tested?

### 5. Benchmark / profiling methodology

- **Claim:** all performance numbers are measured by committed scripts, are synthetic and
  hardware/compiler/build-dependent, and are framed without production claims; dead-code
  elimination and degenerate paths are guarded against.
- **Where:** [`benchmarking.md`](benchmarking.md), [`linux_performance.md`](linux_performance.md),
  [`perf_analysis.md`](perf_analysis.md), `results/` (`latest.txt`, `differential.txt`,
  `allocator_experiment.txt`, `perf_*_linux.txt`).
- **Questions:** Are the microbenchmarks measuring what they claim (no dead-code elimination, real
  fills produced, warmup adequate)? Is the constrained-perf-vs-full-PMU distinction (ADR
  [0007](adr/0007-constrained-perf-artifacts-are-partial-evidence.md)) drawn correctly? Are any
  numbers over-interpreted relative to their caveats?

### 6. Linux / socket profiling methodology

- **Claim:** the gateway syscall/rusage profile separates user-space (matching) cost from
  kernel/socket overhead; the UDP experiment measures receive-buffer-driven loss as
  `published − received` (capturing tail drops); all of it is loopback, constrained evidence.
- **Where:** [`socket_profiling.md`](socket_profiling.md), [`socket_hardening.md`](socket_hardening.md),
  [`socket_gateway.md`](socket_gateway.md), `scripts/profile_gateway_io.sh`,
  `scripts/socket_stress.sh`, `results/socket_profile_loopback.txt`,
  `results/socket_stress_summary.txt`, ADR
  [0008](adr/0008-socket-evidence-loopback-constrained-epoll-deferred.md).
- **Questions:** Is the procfs user/system CPU split a fair way to attribute matching vs
  kernel/socket cost? Is the `strace -c` mix interpreted correctly (and is the launch-form +
  ptrace_scope reasoning right)? Is `published − received` the correct loss metric, and are the
  loopback limitations stated strongly enough? Do the scripts fail safely (no misleading artifact)
  on the error paths they claim to guard?

### 7. CPU locality, false sharing, and concurrency architecture

- **Claim:** CPU-affinity/NUMA and false-sharing work must produce hardware- and
  environment-labeled evidence, and must keep memory-ordering correctness separate from
  performance observations.
- **Where:** [`linux_performance.md`](linux_performance.md),
  [`concurrency_model.md`](concurrency_model.md), [`memory_ordering.md`](memory_ordering.md),
  `MILESTONES.md` (M43/M44).
- **Questions:** Are the CPU-affinity, scheduler-migration, and cache-line contention experiments
  scoped enough to be credible? Are unsupported-host caveats strong enough? Does the M44
  packed-vs-padded cursor study avoid implying production latency or changing queue semantics?

### 8. Storage architecture and cache locality

- **Claim:** allocator and storage evidence is intentionally staged: M28 isolated allocator
  mechanics, M32 PMR node allocation, PR #112 intrusive pooled resting-order storage, and M47
  fixed-band contiguous storage/cache-locality study.
- **Where:** [`pool_backed_storage.md`](pool_backed_storage.md),
  [`allocator_experiment.md`](allocator_experiment.md), `results/pool_backed_storage.txt`,
  `MILESTONES.md` (M47).
- **Questions:** Does the documentation clearly distinguish node-allocation experiments from the
  bounded-domain contiguous architecture? Are replay-equivalence requirements sufficient before
  comparing flat/direct-price-index layouts? Are cache-locality claims forbidden until measured?

### 9. External review priority

- **Claim:** independent technical review remains one of the highest remaining credibility signals,
  ahead of speculative DPDK/NIC research.
- **Where:** this file, [`review_feedback.md`](review_feedback.md), `MILESTONES.md`, `HANDOFF.md`.
- **Questions:** Is the self-certified status visible enough? Are the review questions targeted at
  the risks that matter most, or are there higher-value review areas missing?

## How to give feedback

1. Open an issue using the
   [review-request template](../.github/ISSUE_TEMPLATE/review_request.md), naming the area above
   and the specific claim or file.
2. Or open a PR with the fix and a short rationale.
3. The maintainer records the outcome (accepted / rejected + rationale + follow-up) in
   [`review_feedback.md`](review_feedback.md), so the review history stays honest and auditable.

Process and security context: [`CONTRIBUTING.md`](../CONTRIBUTING.md),
[`SECURITY.md`](../SECURITY.md).

## Review status

- **Self-certified:** everything. The maintainer wrote and checked all of it; correctness rests on
  the committed tests, sanitizers (ASan/UBSan, and ThreadSanitizer for the concurrent tests),
  differential testing against the independent OCaml engine, and the reasoning in the linked docs.
- **Externally reviewed:** **nothing yet.** No external review has been received. This file does
  **not** claim otherwise, and the repository must not present any claim as externally validated
  until a real review is recorded in [`review_feedback.md`](review_feedback.md).

Dynamic analysis (sanitizers, TSan, stress, differential testing) is **empirical evidence over
executed cases**, not a proof of correctness over all inputs or thread interleavings — a point a
reviewer is explicitly invited to push on.

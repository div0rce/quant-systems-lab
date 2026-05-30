# MILESTONES.md — Quant Systems Lab build plan

Sequential, dependency-ordered. **Build them in order.** Each milestone is one feature branch and one squash-merge PR. Do not skip ahead; later milestones assume earlier ones exist.

## Conventions

- Branch: `feat/mNN-slug`
- PR title becomes the single squashed commit on `main`, so it must read cleanly.
- DoD = Definition of Done.
- A milestone is complete only when all DoD boxes are satisfiable, `make check` passes, and `PROGRESS.md` is updated.
- No benchmark numbers may be written into README or résumé bullets until M11 produces them.

---

## M0 — Scaffold, tooling, CI

- **Branch:** `feat/m00-scaffold`
- **PR title:** `chore: scaffold C++ project, tooling, and CI`
- **Goal:** Create a professional empty repo that builds, tests, formats, and runs CI.

### Scope

- CMake project.
- Makefile.
- CMake presets.
- clang-format.
- clang-tidy.
- pre-commit.
- GitHub Actions CI.
- Basic test executable.
- Docs skeleton.
- `.claude/commands/{resume,start-milestone,finish-milestone,review}.md`.
- `.claude/settings.json`.
- Root state files.
- Initial `README.md` with honest project positioning.
- `data/synthetic/README.md` stating data is synthetic.
- `results/README.md` explaining benchmark results policy.

### Definition of Done

- [ ] `make configure` works.
- [ ] `make build` works.
- [ ] `make test` works.
- [ ] `make check` works.
- [ ] CI passes.
- [ ] `no-commit-to-branch` protects `main` locally if feasible.
- [ ] Repo tree matches `CLAUDE.md` target layout closely enough for M1.
- [ ] `PROGRESS.md` updated.

---

## M1 — Core exchange domain types and invariants

- **Branch:** `feat/m01-core-domain`
- **PR title:** `feat: core exchange domain types and invariants`
- **Goal:** Define foundational deterministic exchange types.

### Scope

- `include/qsl/core/types.hpp`
- `include/qsl/core/clock.hpp`
- `include/qsl/core/result.hpp`
- `include/qsl/core/invariants.hpp`
- Side enum.
- Order type enum.
- Time-in-force enum.
- Reject reason enum.
- Domain structs.
- Logical clock.
- Invariant helpers.
- Tests.
- Docs explaining integer ticks and determinism.

### Definition of Done

- [ ] All core types compile.
- [ ] No floating point price types.
- [ ] Logical timestamp exists.
- [ ] Enum conversion and validation tests exist.
- [ ] Invalid domain values reject deterministically.
- [ ] Docs explain price ticks and deterministic time.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M2 — Binary protocol encoding and decoding

- **Branch:** `feat/m02-binary-protocol`
- **PR title:** `feat: binary protocol encoding and decoding`
- **Goal:** Build a fixed-width binary protocol codec.

### Scope

- Message header.
- Byte-order helpers.
- Message structs.
- Encoder.
- Decoder.
- Malformed frame handling.
- Deterministic byte fixtures.
- Protocol docs.

### Definition of Done

- [ ] Encode/decode round-trip tests pass.
- [ ] Malformed frames reject deterministically.
- [ ] Version mismatch rejects.
- [ ] Body length mismatch rejects.
- [ ] Unknown message type rejects.
- [ ] Deterministic byte fixture test exists.
- [ ] `docs/binary_protocol.md` documents frame layout.
- [ ] No undefined behavior from struct reinterpret casts; serialization is explicit.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M3 — Price-time priority order book

- **Branch:** `feat/m03-order-book`
- **PR title:** `feat: price-time priority order book`
- **Goal:** Implement deterministic single-symbol limit order book.

### Scope

- Price levels.
- FIFO order queues.
- Add limit order.
- Market order matching.
- Cancel order.
- Modify order.
- Trade event generation.
- Book snapshot.
- Unit tests.
- `docs/matching_rules.md`.

### Definition of Done

- [ ] Price-time priority works.
- [ ] Partial fills work.
- [ ] Market orders work.
- [ ] Cancel removes resting order.
- [ ] Modify behavior documented and tested.
- [ ] Quantity reduction priority behavior explicitly tested.
- [ ] Price or quantity-increase priority loss explicitly tested.
- [ ] No crossed book after matching.
- [ ] Scenario tests cover common cases.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M4 — Multi-symbol matching engine

- **Branch:** `feat/m04-matching-engine`
- **PR title:** `feat: multi-symbol matching engine`
- **Goal:** Wrap order books into a multi-symbol engine with deterministic sequencing.

### Scope

- Symbol registry.
- Per-symbol books.
- Command application.
- Engine event stream.
- Sequence assignment.
- Deterministic snapshots.
- Integration tests.

### Definition of Done

- [ ] Multiple symbols can trade independently.
- [ ] Sequence numbers strictly increase.
- [ ] Commands produce deterministic event lists.
- [ ] Snapshot comparison exists for replay tests later.
- [ ] Integration tests cover multi-symbol flows.
- [ ] Core engine still does not depend on wall-clock time.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M5 — Deterministic risk checks and in-process order gateway

- **Branch:** `feat/m05-risk-gateway`
- **PR title:** `feat: deterministic risk checks and in-process order gateway`
- **Goal:** Add risk validation before engine application.

### Scope

- Risk config.
- Reject reason codes.
- Duplicate order detection.
- Max quantity check.
- Max notional check.
- Unknown symbol check.
- Invalid tick check.
- In-process gateway.
- Tests.

### Definition of Done

- [ ] Invalid commands reject before reaching engine.
- [ ] Rejections are structured.
- [ ] Accepted commands are forwarded.
- [ ] Duplicate order IDs are rejected.
- [ ] Cancel unknown order rejects.
- [ ] Modify unknown order rejects.
- [ ] Risk tests cover every rejection code.
- [ ] Gateway integration test exists.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M6 — Market data event publisher

- **Branch:** `feat/m06-market-data`
- **PR title:** `feat: market data event publisher`
- **Goal:** Transform engine events into market data messages.

### Scope

- Trade messages.
- Top-of-book updates.
- Book deltas.
- Snapshots.
- Subscriber interface.
- Deterministic publisher sequence.
- Tests.

### Definition of Done

- [ ] Trades produce market data trade messages.
- [ ] Book changes produce top-of-book/delta messages.
- [ ] Publisher sequence is monotonic.
- [ ] Market data can be encoded with binary protocol.
- [ ] Tests verify ordering and content.
- [ ] Publisher sequence follows engine event sequence.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M7 — Append-only event log

- **Branch:** `feat/m07-event-log`
- **PR title:** `feat: append-only event log`
- **Goal:** Persist commands and events in replayable format.

### Scope

- Log record format.
- File writer.
- File reader.
- Payload framing.
- Optional checksum.
- CLI inspection utility.
- Tests.
- `docs/replay_and_recovery.md` initial section.

### Definition of Done

- [ ] Event log appends records.
- [ ] Reader reconstructs records.
- [ ] Corrupted/truncated logs fail safely.
- [ ] Log format documented.
- [ ] Write/read round-trip tests exist.
- [ ] No update-in-place log behavior exists.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M8 — Deterministic replay and recovery

- **Branch:** `feat/m08-replay-recovery`
- **PR title:** `feat: deterministic replay and recovery`
- **Goal:** Rebuild engine state from event log.

### Scope

- Replay engine.
- Snapshot comparison.
- Recovery CLI.
- Deterministic synthetic flow generator.
- Replay integration tests.
- Replay/recovery docs.

### Definition of Done

- [ ] Replay final state equals original final state.
- [ ] Replay emitted events match expected sequence.
- [ ] Recovery CLI can rebuild from log.
- [ ] Test uses deterministic random flow.
- [ ] Docs explain recovery model and limitations.
- [ ] Comparison includes best bid/ask, resting order counts, aggregate price-level quantities, trade sequence, and last sequence number.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M9 — TCP binary order gateway

- **Branch:** `feat/m09-tcp-gateway`
- **PR title:** `feat: TCP binary order gateway`
- **Goal:** Expose order gateway over TCP.

### Scope

- TCP server.
- Session handling.
- Frame read/write.
- Client CLI.
- Heartbeat.
- Graceful disconnect.
- Integration test if feasible.
- Local demo docs.

### Definition of Done

- [ ] Client can send binary order.
- [ ] Server responds with ack/reject/fill.
- [ ] Malformed frames do not crash server.
- [ ] Gateway uses same protocol codec.
- [ ] Heartbeat round trip works.
- [ ] Docs include local demo command.
- [ ] Single-threaded event loop is acceptable; avoid thread soup.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M10 — Network market data publisher

- **Branch:** `feat/m10-network-market-data`
- **PR title:** `feat: network market data publisher`
- **Goal:** Expose market data feed over network.

### Scope

- UDP publisher or TCP subscriber feed.
- Market data client.
- Snapshot request optional.
- Sequence gap detection.
- Tests where feasible.
- Demo docs.

### Definition of Done

- [ ] Market data client receives trade/top-of-book messages.
- [ ] Sequence numbers allow gap detection.
- [ ] Feed uses binary protocol.
- [ ] Local demo works.
- [ ] Tests cover encoding and sequence behavior.
- [ ] Docs state networking limitations honestly.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M11 — Benchmarks and performance reporting

- **Branch:** `feat/m11-benchmarks`
- **PR title:** `perf: benchmark matching, protocol, gateway, and replay`
- **Goal:** Produce reproducible performance results.

### Scope

- Google Benchmark or custom benchmark harness.
- Order book latency benchmark.
- Matching throughput benchmark.
- Protocol encode/decode benchmark.
- Replay benchmark.
- Synthetic flow generator.
- Results summarizer.
- Benchmark docs.
- Benchmark output under `results/`.

### Definition of Done

- [ ] `make bench` runs locally.
- [ ] Benchmark scripts write results under `results/`.
- [ ] README only references measured numbers.
- [ ] Results include hardware/compiler/build info.
- [ ] No fake performance claims.
- [ ] Benchmark methodology documented.
- [ ] Deterministic flow seeds documented.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated with measured results summary.

---

## M12 — Hardening with sanitizers and invariant tests

- **Branch:** `feat/m12-hardening`
- **PR title:** `test: harden engine with sanitizers and invariant tests`
- **Goal:** Make the repo defensible under technical questioning.

### Scope

- ASan/UBSan preset.
- Randomized deterministic flow tests.
- Invariant test suite.
- Protocol fuzz-style malformed input tests.
- Replay stress test.
- CI sanitizer job if feasible.
- Docs listing invariants.

### Definition of Done

- [ ] Sanitizer build passes.
- [ ] Invariant tests pass.
- [ ] Malformed protocol inputs do not crash.
- [ ] Replay stress test passes.
- [ ] Docs list tested invariants.
- [ ] No floating-point prices introduced.
- [ ] Core engine remains wall-clock independent.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## M13 — Final architecture, demo, and recruiting polish

- **Branch:** `feat/m13-docs-polish`
- **PR title:** `docs: final architecture, demo, and recruiting polish`
- **Goal:** Make the repo readable by a recruiter and defensible by an engineer.

### Scope

- README rewrite.
- Architecture diagram.
- Demo script.
- Limitations.
- Benchmark summary.
- Resume bullets.
- Interview defense notes.
- Screenshots or terminal recordings optional.

### Definition of Done

- [ ] README explains the system in under 60 seconds.
- [ ] Quickstart works from clean clone.
- [ ] Architecture diagram committed.
- [ ] Demo script works.
- [ ] Limitations are honest.
- [ ] Resume bullets included.
- [ ] No overclaiming.
- [ ] Benchmark values match M11 outputs exactly.
- [ ] `make check` passes.
- [ ] `PROGRESS.md` updated.

---

## Backlog — optional after M13 only

Do not pull backlog items into earlier PRs.

- Lock-free queue / ring-buffer internals.
- Memory pool allocator.
- Multithreaded gateway and market data pipeline.
- ThreadSanitizer coverage.
- More realistic synthetic order-flow model.
- FIX-like text protocol adapter.
- Web dashboard for visualization.
- Docker packaging.
- Perf/flamegraph docs.
- GitHub Pages documentation site.

### Differential-testing follow-ups (prioritized)

**Definitely track — differential oracle self-test.** Deliberately inject a known C++≠OCaml
mismatch and assert end-to-end: (1) the differential test fails, (2) the failure is detected
correctly, (3) the shrinker reduces the failing stream, (4) the resulting minimal fixture
reproduces the mismatch. This tests the fire alarm, not just the building.

High:

- CI seed sweep: generate seeds 1..N dynamically in CI instead of relying on only the 8
  committed property seeds — stronger differential coverage.
- Dedicated negative fixtures for `best_bid`, `best_ask`, `trades` (trade_count), and bid-side
  `level` lines (today only ask-level qty, `last_seq`, and `order_count` are covered) — cheap,
  improves oracle robustness.
- Synthetic divergence demonstration: show the shrinker finding a real C++≠OCaml failure rather
  than only the artificial "produces a trade" predicate.

Medium:

- Shared gateway-replay helper to remove the duplicated command-dispatch logic in `fixture.cpp`,
  `shrink.cpp`, and the exporters.
- Price simplification in the shrinker (alongside quantity) for smaller counterexamples.
- Symbol/id renumbering shrink pass (could reduce fixtures further than 123 -> 5).
- Generator coverage reporting: track reject-reason frequencies automatically in CI.
- Explicit determinism test across compilers/platforms (currently only indirectly validated by
  the Linux golden check against macOS-committed fixtures).

Low:

- Larger committed corpus (e.g. prop_seed1..50) — more confidence, lower signal per maintenance.
- Performance benchmarks for the differential harness.


---

# Additive Jane Street Milestone Context

The original milestones above remain valid and should not be removed. The following context adds Jane Street SWE/Linux positioning and one additional milestone.

## Cross-milestone Jane Street alignment requirements

Apply these throughout all milestones:

1. Preserve the core C++20 exchange simulator scope.
2. Keep matching deterministic.
3. Keep prices as integer ticks.
4. Avoid fake trading claims.
5. Document Linux/socket/replay tradeoffs as they appear.
6. Do not put benchmark numbers in docs unless generated by scripts.
7. Add OCaml only as a late replay-verifier subproject.
8. Optimize for Jane Street Software Engineering first and Linux Engineering second.

## Milestone documentation additions

When relevant, add or update:

```text
docs/linux_performance.md
docs/socket_gateway.md
```

These docs should evolve gradually. They may start as placeholders in M0, but should become substantive by M9–M13.

## M14 — OCaml replay verifier

- **Branch:** `feat/m14-ocaml-replay-verifier`
- **PR title:** `feat: OCaml replay verifier for exchange event logs`

### Goal

Add a small, disciplined OCaml subproject that validates exported exchange event logs using immutable state transitions. This is a targeted Jane Street SWE signal. It does not replace the C++ engine.

### Scope

Create:

```text
ocaml/
  dune-project
  bin/
    verify_replay.ml
  lib/
    event.ml
    parser.ml
    replay.ml
    invariant.ml
  test/
    test_replay.ml
```

Implement:

1. OCaml event types as algebraic data types.
2. Parser for exported normalized event-log fixtures from the C++ engine.
3. Immutable replay state.
4. Invariant checks:
   - sequence monotonicity,
   - no negative quantity,
   - canceled order cannot later trade,
   - rejected order cannot rest,
   - final snapshot equivalence where fixture includes expected snapshot.
5. CLI:

```text
ocaml/bin/verify_replay.ml <event-log-fixture>
```

6. Dune build and tests.
7. README or docs section explaining why OCaml is used here.

### Definition of Done

- [ ] `dune build` passes inside `ocaml/`.
- [ ] `dune runtest` passes inside `ocaml/`.
- [ ] OCaml verifier parses at least one exported fixture from the C++ event-log pipeline.
- [ ] Replay is immutable and deterministic.
- [ ] Invariant failures produce clear error messages.
- [ ] Docs explain the verifier as a Jane Street language/culture signal without exaggerating OCaml expertise.
- [ ] Root README references the OCaml verifier only after it exists.
- [ ] `PROGRESS.md` updated.

## M15 — Export normalized command streams + final snapshots

- **Branch:** `feat/m15-export-command-streams-and-snapshots`
- **PR title:** `feat: export normalized command streams and final snapshots`

### Goal

Move beyond log-invariant checking by exporting the complete data needed for independent replay: normalized command streams and final C++ engine snapshots.

This replaces the prior optional Jane Street application-polish milestone. Application polish is not the goal. Technical comparability is the goal.

### Scope

Add or extend tooling so the C++ side can export deterministic fixtures containing:

1. symbol registration order;
2. every submitted command in normalized order;
3. every emitted engine event;
4. every structured rejection;
5. final per-symbol snapshot:
   - best bid/ask;
   - per-price aggregate bid levels;
   - per-price aggregate ask levels;
   - resting order counts;
   - last sequence number;
   - trade count;
6. stable text or JSON schema documented in `docs/differential_testing.md`.

### Non-goals

- Do not build the OCaml replay engine here.
- Do not add random generation yet.
- Do not add résumé-polish documents.

### Definition of Done

- [ ] C++ fixture exporter emits normalized command stream fixtures.
- [ ] C++ fixture exporter emits final snapshots with per-symbol level aggregates.
- [ ] Fixture schema is documented.
- [ ] At least one committed fixture exists.
- [ ] Existing M14 OCaml verifier still passes.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] `PROGRESS.md` updated.

## M16 — Independent OCaml replay engine

- **Branch:** `feat/m16-independent-ocaml-replay-engine`
- **PR title:** `feat: independently replay command streams in OCaml`

### Goal

Upgrade the OCaml verifier from log-invariant checker to independent replay engine.

The OCaml side should consume exported command streams, replay the market state immutably, and compute its own final snapshot without trusting the C++ engine's event output.

### Scope

Implement OCaml modules for:

1. command ADTs;
2. symbol registry replay;
3. immutable per-symbol order book;
4. price-time priority matching;
5. active-order lifetime tracking;
6. risk/rejection semantics matching the C++ gateway where fixture data requires it;
7. final snapshot computation.

### Required semantic alignment

- OrderId uniqueness is active-lifetime scoped, not global.
- Integer price ticks only.
- Maker-price fills.
- GTC rests remainder.
- IOC discards remainder.
- Market orders never rest.
- Duplicate active IDs reject/no-op according to the exported fixture semantics.
- Invalid commands must not mutate replay state.

### Non-goals

- Do not optimize OCaml performance.
- Do not claim formal verification.
- Do not reimplement networking.

### Definition of Done

- [ ] OCaml parses normalized command-stream fixtures from M15.
- [ ] OCaml independently replays command streams into immutable state.
- [ ] OCaml computes final snapshot without reading the C++ final snapshot during replay.
- [ ] Unit tests cover matching, cancel, modify, IOC, market, duplicate ID, rejected command, and ID reuse cases.
- [ ] `dune build --root ocaml` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] Docs state exact limitations.
- [ ] `PROGRESS.md` updated.

## M17 — Differential replay tests: C++ vs OCaml snapshot equality

- **Branch:** `feat/m17-differential-replay-tests`
- **PR title:** `test: compare C++ and OCaml replay snapshots`

### Goal

Turn the project into a cross-language differential testing system.

For fixed fixtures, the C++ engine and independent OCaml replay engine must converge to the same final snapshot.

### Scope

Add test harnesses that:

1. generate or load C++ normalized fixtures;
2. run the OCaml replay CLI over them;
3. compare OCaml-computed snapshots against C++-exported snapshots;
4. fail with clear diffs on mismatch;
5. run in CI.

### Definition of Done

- [ ] At least one deterministic C++ fixture is checked by OCaml in CI.
- [ ] Snapshot equality includes per-symbol best bid/ask, level aggregates, order counts, last sequence number, and trade count.
- [ ] A deliberately bad fixture fails with a readable diff.
- [ ] CI has a stable differential-replay job or extends the existing OCaml job.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] `PROGRESS.md` updated.

## M18 — Property-based command generator

- **Branch:** `feat/m18-property-command-generator`
- **PR title:** `test: generate property-based market command streams`

### Goal

Add the deep testing idea: randomized command generation for market-event/state-machine testing.

This is the milestone that moves the project closer to the Jane Street intern-project pattern: not merely building a simulator, but building machinery that attacks the simulator.

### Scope

Implement a deterministic generator for command streams covering:

1. symbol registration;
2. valid limit and market orders;
3. invalid prices and quantities;
4. duplicate active IDs;
5. reusable inactive IDs;
6. unknown symbols;
7. cancels of active and inactive orders;
8. modifies that preserve priority;
9. modifies that lose priority;
10. IOC edge cases;
11. empty-book market orders;
12. multi-symbol interleavings.

### Required properties

Generated streams must be replayable by both C++ and OCaml. Seeds must be printed and saved on failure.

### Definition of Done

- [ ] Generator is deterministic by seed.
- [ ] Generator produces non-vacuous flows: trades, rejects, cancels, modifies, rests, and multi-symbol activity.
- [ ] Generated fixtures feed the C++ engine and OCaml replay path.
- [ ] Property tests assert snapshot equality and core invariants.
- [ ] Failing seeds are reported clearly.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] `PROGRESS.md` updated.

## M19 — Shrinker + minimal failing fixture exporter

- **Branch:** `feat/m19-shrinker-minimal-failing-fixtures`
- **PR title:** `test: shrink failing command streams to minimal fixtures`

### Goal

Add shrinking so randomized failures produce small, reviewable counterexamples rather than useless thousand-command blobs.

This is the strongest testing-systems signal in the project.

### Scope

Implement shrink strategy for failing command streams:

1. remove contiguous chunks;
2. remove single commands;
3. simplify command fields where valid:
   - lower quantities;
   - simpler prices;
   - fewer symbols;
   - fewer order IDs;
4. preserve the failure predicate;
5. export minimized failing fixture and seed metadata.

### Definition of Done

- [ ] A known failing artificial property shrinks to a small fixture in tests.
- [ ] Shrinker is deterministic.
- [ ] Exported failing fixture can be replayed independently.
- [ ] Shrink report includes original seed, original length, minimized length, and failure reason.
- [ ] Docs explain shrink limitations honestly.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] `PROGRESS.md` updated.

## M20 — Final docs: differential testing architecture

- **Branch:** `feat/m20-differential-testing-docs`
- **PR title:** `docs: document differential replay and property testing architecture`

### Goal

Finalize technical documentation around the actual deep idea: differential replay plus property-based generation and shrinking.

This is not application polish. It is architecture documentation for a testing system.

### Scope

Add or update:

```text
docs/differential_testing.md
docs/property_testing.md
README.md
docs/recruiting_notes.md
PROGRESS.md
```

Document:

1. C++ engine as implementation under test;
2. normalized command stream schema;
3. C++ snapshot schema;
4. independent OCaml replay model;
5. C++ vs OCaml snapshot equality;
6. property-based command generation;
7. shrinking and minimal failing fixtures;
8. what this proves;
9. what it does not prove.

### Definition of Done

- [ ] README explains the differential-testing architecture in under 60 seconds.
- [ ] Docs include an architecture diagram.
- [ ] Docs include one minimized failing-fixture example if M19 produced one.
- [ ] Resume bullets emphasize differential testing, not fake production trading.
- [ ] No overclaiming: not formal verification, not production exchange, not profitable trading.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] `PROGRESS.md` updated.

## M21 — Repository license and maintainer docs

- **Branch:** `feat/m21-repo-license-maintainer-docs`
- **PR title:** `chore: add repository license and maintainer docs`

### Goal

Add the minimal legal and maintainer documentation expected of a serious public technical
repo, without pretending this is a large community project. This is practical hygiene, not
corporate open-source governance theater.

### Scope

- Add MIT `LICENSE`.
- Add `CONTRIBUTING.md`.
- Add `SECURITY.md`.
- Add `CHANGELOG.md`.
- Optionally add `SUPPORT.md` only if it stays short and useful.
- Do not add `CODE_OF_CONDUCT.md`.
- Do not add `GOVERNANCE.md`.
- Do not add a root `ARCHITECTURE.md` unless it is only a short pointer to `docs/architecture.md`.
- Do not package the project.
- Do not create a release.

### Required content

- `LICENSE`: MIT, with `Copyright (c) 2026 Moustafa Nasr`.
- `CONTRIBUTING.md` documents: branch-per-milestone workflow; never work on `main`; `make
  check`; `make asan`; `dune runtest --root ocaml`; `make bench` only for benchmark updates;
  no fabricated performance claims; PRs should be small, scoped, and reviewable; generated
  benchmark numbers must be reproducible from committed scripts.
- `SECURITY.md` documents: no bug bounty; local/demo TCP/UDP services are unauthenticated and
  loopback-focused; do not expose `qsl-gateway` or `qsl-mdfeed` to untrusted networks; report
  issues through GitHub issues or maintainer contact; honest language — this is a systems lab,
  not a hardened production service.
- `CHANGELOG.md` starts with: Unreleased; M15–M20 Phase II differential-testing roadmap; M14
  OCaml replay verifier; M12 sanitizer/fuzz/invariant hardening; M11 measured benchmarks;
  M7/M8 replay log and recovery; M3–M6 matching/risk/market-data core.

### Definition of Done

- [ ] GitHub recognizes the license.
- [ ] Maintainer docs are short, honest, and one-maintainer appropriate.
- [ ] No fake foundation/project-governance language.
- [ ] No community-process theater.
- [ ] README links to the new docs only where useful.
- [ ] `make check` passes.
- [ ] `dune runtest --root ocaml` passes.
- [ ] PR opened and not merged.

## M22 — Release readiness audit

- **Branch:** `feat/m22-release-readiness-audit`
- **PR title:** `docs: audit release readiness`

### Goal

The M13-style final polish pass after Phase II: verify the repo reads cleanly, demos cleanly,
reproduces cleanly, and does not overclaim. Recruiting-safe phrasing through technical
substance, not marketing.

### Scope

- Audit README quickstart, demo, benchmark reproduction, docs links, fixture examples, and the
  final Phase II differential-testing docs.
- Verify the repo still tells a coherent story from M0 through M20.
- Regenerate benchmark numbers only if intentionally updating `results/latest.txt` with a
  clean tree.
- Verify: `make demo`; `make check`; `make asan`; `make bench`; `dune runtest --root ocaml`.
- Confirm all README links resolve.
- Confirm all docs and README avoid forbidden claims: production-grade; formal verification;
  HFT platform; low-latency trading system; real exchange; trading bot; production exchange.
- Confirm benchmark language remains: measured; hardware-dependent; synthetic; reproducible
  from committed scripts.
- Confirm fixture/export/replay docs distinguish: invariant checking; independent OCaml
  replay; differential snapshot comparison; property-based generation; shrinking.
- Confirm demo output is clean, deterministic, and useful.
- Do not create a GitHub release in this milestone unless explicitly instructed.

### Definition of Done

- [ ] Final audit notes recorded in `CHANGELOG.md` or `docs/release_readiness.md`.
- [ ] README quickstart verified.
- [ ] Demo verified.
- [ ] Benchmark reproduction verified or explicitly documented if not rerun.
- [ ] All checks pass.
- [ ] No stale milestone references.
- [ ] No overclaiming.
- [ ] No fake production claims.
- [ ] PR opened and not merged.

## M23 — Optional v0.1.0 release

- **Branch:** `feat/m23-v0-1-0-release`
- **PR title:** `chore: prepare v0.1.0 release notes`
- **Suggested release title:** `v0.1.0 — deterministic exchange systems lab`

### Goal

Prepare a conservative GitHub-only `v0.1.0` release after the technical roadmap and readiness
audit are complete. This is optional and requires explicit human approval.

### Scope

- Only start if M20–M22 are merged and the human explicitly approves a release.
- Create release notes for `v0.1.0`.
- Do not package the project.
- Do not publish to Homebrew, vcpkg, Conan, opam, pip, npm, or Docker.
- GitHub release only.
- Release notes must state this is a deterministic exchange-systems lab / research portfolio
  project, not a production exchange.
- Release notes should summarize: deterministic matching/risk/market-data core; replay/event
  log; TCP/UDP demo networking; measured synthetic benchmarks; sanitizer/fuzz/invariant
  hardening; OCaml verifier / independent replay / differential testing, depending on what is
  complete by then; known limitations.

### Definition of Done

- [ ] `CHANGELOG.md` has a `v0.1.0` section.
- [ ] Release notes are conservative and accurate.
- [ ] Git tag prepared only if the human approves.
- [ ] PR opened and not merged unless the human explicitly says to release.
- [ ] No package publishing.

## Jane Street-specific final acceptance bar

The project is strong enough when a reviewer can quickly infer:

1. The user can build nontrivial C++ systems software.
2. The user understands deterministic state machines.
3. The user understands binary protocols and malformed input handling.
4. The user understands replay/recovery and debugging from logs.
5. The user can test invariants instead of merely demoing happy paths.
6. The user can write basic OCaml and reason functionally.
7. The user can discuss Linux/networking/performance tradeoffs honestly.
8. The user does not lie about trading, production use, or benchmark results.

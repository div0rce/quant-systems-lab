# MILESTONES.md — Quant Systems Lab build plan

Sequential, dependency-ordered. **Build them in order.** Each milestone is one feature branch and one squash-merge PR. Do not skip ahead; later milestones assume earlier ones exist.

## Conventions

- Branch: use the prefix that describes the milestone work, for example `feat/mNN-slug`,
  `test/mNN-slug`, `docs/mNN-slug`, `perf/mNN-slug`, or `refactor/mNN-slug`.
- `/start-milestone` must read the selected milestone's exact `Branch:` value and create that
  branch. The command template in `.claude/commands/start-milestone.md` must follow this rule. If
  the `Branch:` field is missing or ambiguous, stop and ask the human; do not synthesize
  `feat/mNN-*` unless that exact branch is listed.
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

> **Execution status (historical).** This backlog was tracked as GitHub issues #24–#51. Items
> **#34–#51 were completed and merged before v0.1.0** (differential follow-ups, shrinker
> improvements, oracle hardening, larger property corpus, regression archive, and
> differential-harness benchmarks). Several remaining issues are **promoted into the Phase III/IV
> roadmap** (issue → milestone): **#24 → M24** (ring buffer), **#26 → M26** (threaded pipeline),
> **#27 → M27** (ThreadSanitizer), **#25 → M28** (memory-pool allocator), **#32 → M29** (Linux
> perf/flamegraph). M25 (memory-ordering evidence), M30 (socket profiling/hardening), and M31
> (external review) are new milestones with no prior issue. PR #112 closed
> the remaining tractable systems items **#26** (portable TCP serving beyond one-connection-at-a-time
> accept) and **#28** (realistic synthetic order-flow model). The genuinely **deferred** product/API
> items remain **#29** (FIX adapter), **#30** (web dashboard), **#31** (Docker packaging), and
> **#33** (Pages site) — do not start them before the Phase III/IV systems roadmap unless the human
> explicitly reprioritizes.

Do not pull backlog items into earlier PRs.

- Lock-free queue / ring-buffer internals. (#24)
- Memory pool allocator. (#25)
- Multithreaded gateway and market data pipeline, plus portable threaded TCP serving follow-up. (#26)
- ThreadSanitizer coverage. (#27)
- More realistic synthetic order-flow model. (#28)
- FIX-like text protocol adapter. (#29)
- Web dashboard for visualization. (#30)
- Docker packaging. (#31)
- Perf/flamegraph docs. (#32)
- GitHub Pages documentation site. (#33)

### Differential-testing follow-ups (prioritized)

**Definitely track — differential oracle self-test.** Deliberately inject a known C++≠OCaml
mismatch and assert end-to-end: (1) the differential test fails, (2) the failure is detected
correctly, (3) the shrinker reduces the failing stream, (4) the resulting minimal fixture
reproduces the mismatch. This tests the fire alarm, not just the building. (#34)

High:

- CI seed sweep: generate seeds 1..N dynamically in CI instead of relying on only the 8
  committed property seeds — stronger differential coverage. (#35)
- Dedicated negative fixtures for `best_bid`, `best_ask`, `trades` (trade_count), and bid-side
  `level` lines (today only ask-level qty, `last_seq`, and `order_count` are covered) — cheap,
  improves oracle robustness. (#36)
- Synthetic divergence demonstration: show the shrinker finding a real C++≠OCaml failure rather
  than only the artificial "produces a trade" predicate. (#37)
- Differential fixture coverage matrix: a maintained table of every snapshot field × {positive,
  negative, property, shrink} coverage, so blind spots cannot silently reappear. (#38)
- Oracle independence audit: document every place the OCaml oracle could accidentally mirror C++
  implementation details, to evidence that it is genuinely independently implemented. (#39)

Medium:

- Shared gateway-replay helper to remove the duplicated command-dispatch logic in `fixture.cpp`,
  `shrink.cpp`, and the exporters. (#41)
- Price simplification in the shrinker (alongside quantity) for smaller counterexamples. (#42)
- Symbol/id renumbering shrink pass (could reduce fixtures further than 123 -> 5). (#43)
- Generator coverage reporting: track reject-reason frequencies automatically in CI. (#44)
- Explicit determinism test across compilers/platforms (currently only indirectly validated by
  the Linux golden check against macOS-committed fixtures). (#45)
- **Differential failure artifact bundle (highest-value addition):** on a CI divergence, save and
  upload as artifacts the original stream, the shrunk stream, the C++ output, the OCaml output,
  and the unified diff — mirrors mature fuzzing/differential systems and makes debugging easy. (#40)
- Shrinker effectiveness metrics: report original/final command counts, reduction %, and shrink
  iterations during CI. (#46)
- Seed reproducibility manifest: record generator version, seed, and fixture hash so future
  generator changes cannot cause confusion. (#47)

Medium-Low:

- Mutation testing for the oracle: intentionally corrupt snapshots, trade counts, best bid/ask,
  and sequence numbers, and verify the differential layer detects each — validates the checker. (#48)

Low:

- Larger committed corpus (e.g. prop_seed1..50) — more confidence, lower signal per maintenance. (#49)
- Historical regression archive: keep a folder of important shrunk failures discovered over time
  (useful once real bugs are ever found). (#50)
- Performance benchmarks for the differential harness. (#51)


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


---

# Phase III / IV roadmap (post-v0.1.0)

v0.1.0 proved a correctness-first deterministic exchange-systems lab. Phase III/IV is the next
deliberate credibility arc: real concurrency, memory discipline, Linux profiling, socket
hardening, and external review signal — not more product surface area.

## Phase III / IV execution rule

Do M24–M49 **in order**, except that issue #90 can be worked as an evidence follow-up as soon as a
PMU-capable Linux host is available. Do not skip to Linux perf/socket work before the concurrency
primitive and threaded pipeline exist. Do not start external review before there is enough evidence
to review.

1. M24 ring buffer
2. M25 memory-ordering evidence
3. M26 threaded pipeline
4. M27 ThreadSanitizer
5. M28 memory pool allocator
6. M29 Linux perf workflow and constrained profiling validation
7. M30 kernel/socket profiling and socket hardening
8. M31 external review package
9. M32 pool-backed order-book storage experiment
10. M33 advanced concurrency validation
11. M34 epoll gateway architecture
12. M35 multi-client load and socket-pressure testing
13. M36 decompose the epoll event loop and connection lifecycle
14. M37 extract threaded-pipeline stage helpers
15. M38 split the command-stream shrinker into named passes
16. M39 encapsulate order-book matching parameters
17. M40 consolidate engine correctness test suites
18. M41 simplify gateway session frame dispatch
19. M42 extract shared shell-script helpers
20. M43 NUMA awareness study
21. M44 ingress queue memory-ordering and false-sharing study
22. M45 exchange-grade persistence prototype
23. M46 recovery benchmarking
24. M47 contiguous order-book storage and cache-locality study
25. M48 DPDK research and prototype
26. M49 NIC offload and low-latency networking study

## Post-M29 priority order

After PR #89, prioritize:

1. Issue #90 — generate full Linux hardware PMU perf artifacts on a PMU-capable Linux target.
2. M30 — socket/kernel profiling.
3. M31 — external review signal.
4. M32 — pool-backed order-book integration.
5. M33 — advanced concurrency validation.

These items produce more systems-engineering signal than more isolated microbenchmarks.
Future CPU locality, false-sharing, contiguous-storage, and external-review evidence should remain
ahead of speculative DPDK/NIC exploration because they exercise the current codebase directly.

Forbidden throughout: production-grade/HFT/real-exchange/formally-verified/profitable/guaranteed-
low-latency/production-networking claims; and dashboards, trading strategies, market-data APIs,
FIX adapters, Docker packaging, or Pages sites (the deferred #29–#31 and #33) before this arc completes.

## Roadmap audit decisions after PR #112

PR #112 closed the post-M42 follow-ups for issue #95 (intrusive `OrderPool`-backed order-book
storage), issue #28 (more realistic deterministic synthetic order-flow model), and issue #26
(portable TCP serving beyond one-connection-at-a-time). The remaining roadmap now prioritizes
systems-engineering evidence that applies directly to the implemented simulator:

- M43 expands the existing NUMA milestone to include CPU affinity, scheduler migration, and
  core/cache locality evidence.
- M44 already owned ingress contention, so it is expanded in place into memory-ordering and
  false-sharing validation rather than creating a duplicate concurrency milestone.
- M47 is inserted for contiguous order-book storage and cache-locality study because PMR and
  intrusive pooled storage still leave price levels and locators in node-based containers.
- DPDK and NIC offload remain late research milestones, shifted to M48/M49. They are lower
  priority than memory ordering, cache locality, storage architecture, Linux evidence, and
  independent external review.

Rejected additions:

- `mmap` artifact logging: current artifact-bundle/replay evidence is sufficient; the complexity
  is higher than the current systems-signal value.
- Bitwise float sanitization: the project uses integer ticks by design, so the problem is
  intentionally avoided.
- `constexpr` string hashing: low signal relative to concurrency, storage, Linux evidence, and
  review work.
- SIMD liquidation engine: no liquidation engine exists; adding one would be synthetic
  optimization work.

---

## M24 — Bounded SPSC ring buffer

- **Branch:** `feat/m24-spsc-ring-buffer`
- **PR title:** `feat: add bounded SPSC ring buffer`
- **Goal:** A small bounded single-producer/single-consumer queue as the concurrency primitive
  for later threaded pipeline work.

### Scope

- `include/qsl/concurrency/spsc_ring.hpp`
- `tests/unit/test_spsc_ring.cpp`
- `docs/concurrency_model.md` (or `docs/memory_ordering.md`) initial section

### Definition of Done

- [ ] Fixed-capacity bounded queue; no dynamic allocation in `try_push`/`try_pop`.
- [ ] `try_push`, `try_pop`, `empty` (and `full` if useful); deterministic full-queue behavior.
- [ ] Acquire/release memory ordering documented (why, per operation).
- [ ] Tests: empty pop fails; single push/pop; FIFO preservation; wraparound; full queue rejects;
      capacity boundary; single-producer/single-consumer stress.
- [ ] `make check` + `make asan` pass; docs explain why SPSC (not MPMC) and the ordering choices.
- [ ] `PROGRESS.md` updated.

### Non-goals

No MPMC, no thread pool, no premature integration into gateway/engine/feed, no lock-free
marketing language unless the implementation actually qualifies, no benchmark claims unless measured.

---

## M25 — Memory-ordering and concurrency evidence package

- **Branch:** `feat/m25-memory-ordering-evidence`
- **PR title:** `docs: document concurrency ownership and memory ordering`
- **Goal:** Turn M24 from "I wrote a queue" into "I understand the queue."

### Scope

- `docs/concurrency_model.md`, `docs/memory_ordering.md`
- `tests/concurrency/test_spsc_stress.cpp`, `tests/concurrency/test_backpressure.cpp`

### Definition of Done

- [ ] Ownership-model table; SPSC memory-ordering table (producer: load tail acquire/justified,
      write slot normal, publish head release; consumer: load head acquire, read slot normal,
      publish tail release/justified).
- [ ] Producer/consumer visibility explanation; backpressure semantics; shutdown assumptions for
      the later pipeline; false-sharing/cache-line discussion if padding is used.
- [ ] Explicit limits: SPSC only, not a general concurrent container; no "wait-free/lock-free"
      claim unless proven.
- [ ] Docs and tests agree; `make check` + `make asan` pass; `PROGRESS.md` updated.

---

## M26 — Multithreaded gateway-engine-feed pipeline prototype

- **Branch:** `feat/m26-threaded-pipeline`
- **PR title:** `feat: add threaded gateway-engine-feed pipeline prototype`
- **Goal:** A real concurrency boundary without corrupting deterministic engine semantics.
  Architecture: input thread → bounded inbound SPSC queue → engine thread → bounded outbound
  event queue → publisher/log thread.

### Definition of Done

- [ ] Explicit ownership transfer between threads; bounded queues; deterministic full-inbound-queue
      handling; event-log integrity never silently dropped; clean startup/shutdown, all threads joined.
- [ ] Tests: processes commands in order; publisher lag does not corrupt engine state; full inbound
      queue → deterministic backpressure; `shutdown_empty`, `shutdown_with_pending_commands`,
      `shutdown_with_full_queue`; event-log records not silently dropped.
- [ ] `make check` + `make asan` pass; replay/differential tests still pass; architecture docs
      updated; `PROGRESS.md` updated.

### Non-goals

No production matching-latency attempt, no MPMC queue, no kernel bypass, no real-exchange claim.

---

## M27 — ThreadSanitizer coverage

- **Branch:** `feat/m27-thread-sanitizer`
- **PR title:** `test: add ThreadSanitizer coverage for concurrent pipeline`
- **Goal:** TSan as a correctness gate for concurrent code.

### Scope

- `CMakePresets.json`, `cmake/Sanitizers.cmake`, `Makefile`, `.github/workflows/ci.yml`,
  `docs/concurrency_model.md`. Adds `make tsan` / `ctest --preset tsan`.

### Definition of Done

- [ ] TSan preset builds the threaded tests; CI runs a TSan job if feasible (clearly documented if
      unsupported on the local/macOS toolchain).
- [ ] Docs state TSan is for data-race detection, not performance; no benchmark numbers collected
      under TSan.
- [ ] Docs avoid representing TSan as a proof: it validates executed schedules only, not all
      possible thread interleavings.
- [ ] `make tsan` passes where supported; `make check` passes; `PROGRESS.md` updated.

---

## M28 — Memory pool allocator experiment

- **Branch:** `feat/m28-memory-pool-allocator`
- **PR title:** `perf: add memory pool allocator experiment`
- **Goal:** Measure whether controlling hot-path order allocation improves latency/determinism.

### Scope

- `include/qsl/memory/order_pool.hpp` (+ `src/memory/order_pool.cpp` if needed),
  `benchmarks/bench_order_pool.cpp`, `docs/allocator_experiment.md`,
  `results/allocator_experiment.txt` (if script-generated).

### Definition of Done

- [ ] Pool/object allocator for hot-path order-like objects; deterministic capacity/failure
      behavior; no silent fallback unless documented.
- [ ] Benchmark baseline vs pool path; results carry hardware/compiler/build/commit/dirty-tree
      metadata; a negative (honest) result is acceptable.
- [ ] No README number unless generated and committed intentionally; `make check` + the allocator
      benchmark run; `PROGRESS.md` updated.

---

## M29 — Linux perf profiling workflow and artifacts

- **Branch:** `feat/m29-linux-perf-profiling`
- **PR title:** `perf: add Linux perf profiling artifacts`
- **Goal:** Linux perf workflow plus honest constrained-environment validation. Full hardware PMU
  evidence requires a PMU-capable Linux host and is tracked separately by issue #90.

### Scope

- `scripts/perf_stat.sh`, `scripts/perf_record.sh`, `docs/perf_analysis.md`,
  `results/perf_stat_linux.txt`, `results/perf_report_linux.txt`, `results/flamegraph.svg`
  (optional only when reproducible).

### Definition of Done

- [ ] Linux-only scripts fail clearly on non-Linux; record CPU/kernel/compiler/build metadata.
- [ ] `perf stat` and `perf record/report` workflows exist and capture metadata-rich artifacts.
- [ ] Dirty-tree metadata excludes only generated artifacts inside the repo; dirty-check failures do
      not silently record `Dirty tree: no`.
- [ ] If hardware PMU counters are unavailable, artifacts are labeled constrained-environment
      validation and must not be described as full PMU evidence.
- [ ] Full PMU evidence requires `Artifact: hardware PMU evidence`, `Unsupported counters detected:
      no`, `Hardware counters supported: yes`, `Dirty tree: no`, and numeric values for the required
      hardware counters.
- [ ] Issue #90 tracks full hardware PMU evidence generation when a bare-metal or PMU-capable Linux
      VM/server is available.
- [ ] Docs explain what was and was not profiled, what was not optimized, and that results are
      hardware/kernel/compiler dependent; artifacts committed with caveats or regeneration documented.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M30 — Kernel/socket path profiling and Linux socket hardening

- **Branch:** `feat/m30-socket-profiling-hardening`
- **PR title:** `perf: profile and harden Linux socket path`
- **Goal:** Linux/socket competence beyond "TCP/UDP demo compiles."

### Scope

- `scripts/profile_gateway_io.sh`, `scripts/socket_stress.sh`, `docs/socket_profiling.md`,
  `docs/socket_hardening.md`, `results/socket_profile_loopback.txt`, `results/socket_stress_summary.txt`.

### Definition of Done

- [ ] Syscall summary for the gateway/feed path; context-switch/page-fault profile; UDP burst/gap
      experiment; socket-buffer experiment (default / small / larger receive buffer); user-space
      engine cost distinguished from kernel/socket overhead; loopback limitations documented.
- [ ] Optional `epoll` adapter only if cleanly scoped (nonblocking accept/recv/send,
      EAGAIN/EWOULDBLOCK handling, bounded event batch, clean shutdown); `io_uring` may be discussed
      but not implemented unless justified.
- [ ] Scripts run on Linux or skip clearly elsewhere; no production-networking or kernel-bypass
      claim; `make check` + relevant socket tests pass; `PROGRESS.md` updated.

---

## M31 — External review / maintainer signal

- **Branch:** `docs/m31-external-review`
- **PR title:** `docs: prepare external review package`
- **Goal:** A credible external-review surface after the technical evidence exists.

### Scope

- `docs/review_request.md`, `docs/review_feedback.md`,
  `.github/ISSUE_TEMPLATE/review_request.md` (optional), small README link (optional).

### Definition of Done

- [ ] Public review checklist explicitly asking for criticism on: SPSC memory ordering;
      backpressure semantics; threaded ownership model; event-log integrity under concurrency;
      benchmark/profiling methodology; Linux/socket profiling methodology.
- [ ] No fake endorsement language; no claim that review has happened until it has; if feedback
      exists, summarize reviewer / criticism / accepted-rejected / rationale / follow-up.
- [ ] Docs clearly distinguish self-certified vs externally reviewed claims; review request issue
      opened or template prepared; `make check` passes; `PROGRESS.md` updated.

---

## Post-M29 backlog additions

### TSan coverage is evidence, not proof

Current state:

- M27 added ThreadSanitizer coverage.
- TSan validates executed schedules and can detect races that occur during tested executions.
- TSan does not prove correctness across all possible thread interleavings.
- Existing stress tests, queue-capacity sweeps, and TSan runs provide empirical evidence only.

Future work:

- randomized scheduling perturbation;
- expanded stress coverage;
- long-duration Linux stress runs;
- additional happens-before reasoning documentation;
- stronger concurrency validation methodology.

Recruiting significance: improves rigor of concurrency claims, prevents overstating sanitizer
coverage, and demonstrates understanding of dynamic-analysis limits. Do not represent TSan as a
correctness proof anywhere in the repository.

### Pool-backed order-book storage experiment

Current state:

- M28 implemented a raw-storage `OrderPool`.
- M28 benchmarked allocator acquire/release versus `new`/`delete`.
- M28 preserved correct object lifetimes.
- PR #88 did not integrate pool storage into the order-book implementation.
- Matching engine storage architecture is unchanged.
- M28 evidence is allocator evidence, not engine-storage evidence.
- The current order book uses `std::list<Order>` plus `std::map` and `std::unordered_map`.
  `std::list<Order>` allocates implementation-defined list nodes, not bare `engine::Order`
  objects, so direct `OrderPool<Capacity>` integration would require an intrusive/custom-node
  redesign.

Post-M42 follow-up:

- add an explicit intrusive `OrderPool<Capacity>` storage mode for resting orders while preserving
  the baseline and PMR storage paths;
- measure end-to-end matching workloads;
- evaluate cache-locality effects;
- evaluate replay impact;
- compare against arena, intrusive, and flat-container alternatives;
- retain only if evidence supports it.

Recruiting significance: moves allocator experimentation into actual engine-memory architecture
work without pretending the M28 allocator microbenchmark already changed the engine.

---

## M32 — Pool-backed order-book storage experiment

- **Branch:** `feat/m32-pool-backed-order-book-storage`
- **PR title:** `perf: evaluate pool-backed order-book storage`
- **Goal:** Memory-architecture evaluation inside selected order-book paths, not another allocator
  microbenchmark.

### Scope

- Integrate pool-backed order-book node allocation using PMR, informed by the M28 allocator
  experiment.
- Keep direct M28 `OrderPool<Capacity>` integration out of scope; that requires an
  intrusive/custom-node order-book redesign. That direct-storage follow-up was later handled by
  PR #112 rather than by M32 itself.
- Benchmark engine-level workloads against the baseline storage.
- Analyze cache locality and replay impact.
- Compare against arena, intrusive, and flat-container alternatives at the documentation level.
- Document results even if negative.

### Definition of Done

- [ ] Pool-backed storage path is scoped and reversible.
- [ ] Direct intrusive/custom-node `OrderPool<Capacity>` storage is explicitly separated from M32
      and handled by the later PR #112 feature follow-up.
- [ ] Baseline vs integrated engine workload measurements are produced by committed scripts.
- [ ] Results document cache/locality/replay effects and limitations.
- [ ] No README/resume claim unless supported by the measured artifact.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M33 — Advanced concurrency validation

- **Branch:** `feat/m33-advanced-concurrency-validation`
- **PR title:** `test: expand concurrency validation methodology`
- **Goal:** Improve confidence in concurrency correctness without claiming proof.

### Scope

- Randomized scheduling perturbation.
- Stronger stress coverage.
- Longer Linux runs where available.
- Concurrency-analysis documentation.
- Expanded validation methodology.

### Definition of Done

- [ ] Tests add scheduling perturbation or longer stress modes without flakiness in normal CI.
- [ ] Docs distinguish empirical evidence, TSan coverage, and reasoning/proof obligations.
- [ ] Long-running or Linux-only checks are documented with clear opt-in commands.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M34 — epoll gateway architecture

- **Branch:** `feat/m34-epoll-gateway-architecture`
- **PR title:** `feat: add epoll gateway architecture prototype`
- **Goal:** Event-driven gateway design with bounded, documented behavior.

### Scope

- `epoll`-based gateway architecture.
- Multi-client readiness handling.
- Nonblocking accept/read/write with clear disconnect behavior.
- Measurement and documentation.

### Definition of Done

- [ ] Event-driven path handles multiple clients without thread-per-connection design.
- [ ] EAGAIN/EWOULDBLOCK and partial read/write behavior is tested or documented.
- [ ] Existing deterministic session semantics are preserved.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M35 — Multi-client load and socket-pressure testing

- **Branch:** `feat/m35-multi-client-socket-pressure`
- **PR title:** `test: add multi-client socket pressure coverage`
- **Goal:** Stress the gateway/feed network path under realistic local pressure.

### Scope

- Socket-buffer pressure.
- TCP/UDP stress.
- Connection scaling.
- Backpressure investigation.

### Definition of Done

- [ ] Scripts/tests document load shape and environment.
- [ ] Results distinguish kernel/socket pressure from engine costs.
- [ ] No production-capacity claim.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## Repository-health refactor phase (inserted after M35)

After M35, a CodeScene Code Health analysis (project 80913) of all production and test files
identified eleven files below 9.0. They are addressed by the seven behavior-preserving refactor
milestones **M36–M42** below, inserted before the original networking/persistence roadmap (which
shifts to **M43–M49**; NUMA is now **M43**). M36–M39 and M41 come from production findings; M40
consolidates the engine test suites (a CodeScene test finding, kept as its own milestone); M42
(shell-script helpers) was identified manually because CodeScene cannot score shell. Determinism,
replay, the differential suite, and integer-tick pricing remain invariants, not refactor targets.

---

## M36 — Decompose the epoll event loop and connection lifecycle

- **Branch:** `refactor/m36-epoll-event-loop-decomposition`
- **PR title:** `refactor: decompose epoll event-loop and connection lifecycle`
- **Goal:** Reduce the complexity of the epoll transport without changing its behavior.

### Scope

- `src/gateway/epoll_server.cpp` (Code Health 5.35): split `EpollServer::serve_listen_socket`
  (cyclomatic complexity 67, 154 lines, nesting depth 7) into per-event handlers
  (accept / readable / writable / error+hup / close) plus a connection-lifecycle helper.
- Flatten nesting with guard clauses; encapsulate the complex conditionals.

### Definition of Done

- [ ] `serve_listen_socket` decomposed; no function exceeds the C++ complexity/length thresholds.
- [ ] CodeScene Code Health for `src/gateway/epoll_server.cpp` improves to ≥ 9.0 (record before→after).
- [ ] Behavior unchanged: `tests/unit/test_epoll_gateway.cpp` passes; characterization tests cover
      accept / `EAGAIN` / `EPOLLHUP` / `EPOLLERR` / hard-cap / fd-generation paths.
- [ ] No public API or wire-behavior change; session/matching semantics unchanged.
- [ ] `make check` + `make asan` pass; `PROGRESS.md` updated.

---

## M37 — Extract threaded-pipeline stage helpers

- **Branch:** `refactor/m37-threaded-pipeline-stage-helpers`
- **PR title:** `refactor: extract threaded pipeline stage helpers`
- **Goal:** Break up the pipeline's brain method and tidy the concurrency tests that guard it.

### Scope

- `include/qsl/concurrency/pipeline.hpp` (Code Health 7.13): split the `run` brain method
  (103 lines, nesting 4, 5 arguments) into ingress / engine / egress stage helpers and a
  run-context struct.
- `tests/concurrency/test_pipeline.cpp` (8.28) and `tests/concurrency/test_backpressure.cpp` (8.44):
  reduce large/duplicated assertion blocks and per-test nesting via shared helpers.

### Definition of Done

- [ ] `run` decomposed; argument count and nesting within thresholds.
- [ ] Code Health ≥ 9.0 for `pipeline.hpp`, `test_pipeline.cpp`, and `test_backpressure.cpp`.
- [ ] Determinism preserved: the threaded result still equals the single-threaded reference and the
      replay of the concurrently-written command log across seeds and queue capacities.
- [ ] `make check` + `make asan` + `make tsan` pass; `PROGRESS.md` updated.

---

## M38 — Split the command-stream shrinker into named passes

- **Branch:** `refactor/m38-shrinker-reduction-passes`
- **PR title:** `refactor: split shrinker into named reduction passes`
- **Goal:** Make the shrinker's reduction strategy readable without changing its output.

### Scope

- `src/replay/shrink.cpp` (Code Health 8.15): split `shrink` (cc 18) and `renumber` (cc 21) into
  named reduction passes (chunk removal, single-command removal, field simplification) behind the
  existing failure predicate; extract the id-remap helper.

### Definition of Done

- [ ] `shrink` / `renumber` decomposed; functions within complexity/length thresholds.
- [ ] Code Health for `src/replay/shrink.cpp` ≥ 9.0.
- [ ] Deterministic shrink output byte-identical (committed shrunk fixtures unchanged); shrinker tests
      and the OCaml differential suite stay green.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M39 — Encapsulate order-book matching parameters

- **Branch:** `refactor/m39-order-book-matching-parameters`
- **PR title:** `refactor: encapsulate order-book matching parameters`
- **Goal:** Reduce order-book complexity in the determinism-critical matching path.

### Scope

- `src/engine/order_book.cpp` (Code Health 8.55): collapse `match_against` (7 arguments, cc 12) and
  `count_matches` (5 arguments) into a matching-context struct; extract the fill loop. Behavior must
  stay byte-identical.

### Definition of Done

- [ ] `match_against` / `count_matches` argument counts and complexity within thresholds.
- [ ] Code Health for `src/engine/order_book.cpp` ≥ 9.0.
- [ ] **Determinism preserved:** replay equivalence, the C++↔OCaml differential suite, and all
      property/invariant tests stay green (identical event stream, snapshot, and `last_seq`).
- [ ] Integer-tick prices and wall-clock-independent matching unchanged.
- [ ] `make check` + `make asan` pass; `PROGRESS.md` updated.

---

## M40 — Consolidate engine correctness test suites

- **Branch:** `refactor/m40-engine-test-consolidation`
- **PR title:** `test: consolidate engine correctness test suites`
- **Goal:** Remove duplication and oversized assertion blocks from the engine/risk test suites
  without weakening coverage. Test-only milestone — no production change.

### Scope

- `tests/unit/test_order_book.cpp` (7.32), `tests/unit/test_matching_engine.cpp` (8.54),
  `tests/unit/test_invariants.cpp` (8.45), `tests/unit/test_risk_gateway.cpp` (6.69): extract shared
  fixtures / assertion helpers, de-duplicate `TEST_CASE`s, and break up large/duplicated assertion
  blocks and deeply nested property loops.

### Definition of Done

- [ ] Code Health ≥ 9.0 for all four files.
- [ ] No production code changed; the same scenarios and invariants are asserted (coverage and
      non-vacuity preserved — trades/rejects/cancels/modifies still exercised).
- [ ] `make check` + `make asan` pass; `PROGRESS.md` updated.

---

## M41 — Simplify gateway session frame dispatch

- **Branch:** `refactor/m41-session-frame-dispatch`
- **PR title:** `refactor: simplify gateway session frame dispatch`
- **Goal:** Replace the session's complex frame-dispatch method with a clear per-message structure.

### Scope

- `src/gateway/session.cpp` (Code Health 8.99): replace `Session::process_frame` (cc 15) message-type
  if/else chain with a per-message-type handler table / functions.

### Definition of Done

- [ ] `process_frame` complexity within threshold; Code Health for `src/gateway/session.cpp` ≥ 9.0.
- [ ] Behavior unchanged: malformed-frame disconnect, risk-reject, cancel, modify, and chunked-read
      session tests pass.
- [ ] `make check` + `make asan` pass; `PROGRESS.md` updated.

---

## M42 — Extract shared shell-script helpers

- **Branch:** `refactor/m42-shared-shell-script-helpers`
- **PR title:** `refactor: extract shared shell-script helpers`
- **Goal:** Remove duplicated boilerplate across the socket/perf scripts (the M35 deferred follow-up).
  Manually identified — CodeScene does not score shell, so this milestone has no Code Health metric.

### Scope

- Extract the shared dirty-tree / `wait_ready` / gateway-stop / metadata-emission helpers duplicated
  across `scripts/socket_load.sh`, `scripts/socket_stress.sh`, `scripts/profile_gateway_io.sh`,
  `scripts/perf_record.sh`, and `scripts/perf_stat.sh` into a sourced `scripts/` library.

### Definition of Done

- [ ] Shared helpers live in one sourced library; the five scripts source it (no copy-paste).
- [ ] `bash -n` clean for every script; each script's artifact output is unchanged.
- [ ] The Linux `make` targets (`socket-load`, `socket-stress`, `profile-io`, `perf-stat`,
      `perf-record`) still run; `make check` passes; `PROGRESS.md` updated.

---

## M43 — NUMA awareness study

- **Branch:** `feat/m43-numa-awareness-study`
- **PR title:** `docs: study NUMA and CPU affinity effects`
- **Goal:** Document and measure CPU locality tradeoffs where hardware exists.
- **Status:** ☑ merged via PR #114 (`29ed491`).
- **Dependencies:** M29/M30 Linux profiling workflow, M35 socket-load evidence, and PR #112
  storage/TCP follow-up. A Linux host with suitable topology information is required for full
  evidence, but constrained-host runs may still proceed when labeled honestly.
- **Signal gained:** Scheduler behavior, core locality, NUMA/CPU-affinity reasoning, and honest
  Linux performance engineering.
- **Evidence required:** Metadata-rich artifacts or documented constrained-host output that record
  hardware topology, kernel, compiler/build, git commit, command lines, dirty-tree state, pinned
  and unpinned scheduler counters where available, and whether node-local/remote NUMA binding
  actually succeeded.

### Scope

- CPU affinity experiments using `taskset` where available and `pthread_setaffinity_np` only if a
  scoped code probe is justified.
- Scheduler migration measurements and documentation.
- Core pinning / isolated-core workflow notes.
- NUMA locality experiments on NUMA-capable Linux hosts.
- Cache-locality discussion that distinguishes core locality from storage-layout locality.
- Unsupported-host caveats for macOS, Docker Desktop, non-NUMA Linux, and restricted CI.

### Definition of Done

- [ ] Scripts or docs describe how to run the study on NUMA-capable Linux.
- [ ] CPU affinity and scheduler-migration commands are documented and, where hardware permits,
      measured before/after.
- [ ] Artifacts are labeled hardware-specific.
- [ ] The artifact self-classifies as `full-linux-numa`, `linux-constrained`, or
      `unsupported-host`.
- [ ] Full NUMA evidence requires successful local/remote NUMA binding plus pinned and unpinned
      migration/context-switch counter capture; single-CPU pinning alone is constrained evidence.
- [ ] Non-NUMA hosts are treated as unsupported/constrained, not faked.
- [ ] Docs forbid production-latency claims and distinguish topology evidence from optimization
      claims.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M44 — Ingress queue memory-ordering and false-sharing study

- **Branch:** `feat/m44-ingress-memory-ordering-false-sharing`
- **PR title:** `perf: study ingress memory ordering and false sharing`
- **Goal:** Evaluate ingress queue contention, memory ordering, and cache-line sharing without
  changing deterministic matching ownership.
- **Status:** ◐ PR-ready.
- **Dependencies:** M24 SPSC ring, M25 memory-ordering docs, M26 threaded pipeline, M27 TSan, M33
  advanced concurrency validation, and M37 pipeline helper refactor.
- **Signal gained:** Acquire/release reasoning, queue ownership, cache-line contention evidence,
  and concurrency-architecture clarity.
- **Evidence required:** A committed benchmark or stress artifact with metadata, plus docs that
  explain whether padding/alignment changed behavior or only measured contention shape. If the
  study is benchmark-only, it must explicitly say the production `SpscRing` layout and matching
  ownership were not changed.

### Scope

- Existing ingress queues, backpressure, and SPSC ownership model.
- False-sharing benchmark or stress mode comparing packed vs cache-line-padded queue indices or
  counters.
- Optional `alignas(64)` padding experiment only when guarded by tests and documentation.
- Acquire/release ordering re-validation after any padding/layout change.
- Shared-cache contention discussion and scheduler-noise caveats.

### Non-goals

- Not lock-free matching.
- Not MPMC.
- Not a production throughput or latency claim.
- A weak/fake implementation would add padding without measuring packed-vs-padded behavior or
  without documenting the concurrency ownership model.

### Definition of Done

- [ ] Matching engine ownership remains deterministic and single-owner where required.
- [ ] Any lock-free/wait-free wording is scoped to the queue protocol and payload assumptions only.
- [ ] False-sharing or shared-cache contention measurements are produced by committed scripts, or
      the host limitation is recorded honestly.
- [ ] Docs distinguish memory-ordering correctness from performance evidence.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M45 — Exchange-grade persistence prototype

- **Branch:** `feat/m45-persistence-prototype`
- **PR title:** `feat: prototype stronger persistence strategy`
- **Goal:** Investigate durability strategy beyond the current append-only lab log.
- **Status:** ☐ not started.
- **Dependencies:** M7/M8 event log and replay plus M30 socket/kernel profiling. M45 should define
  durability/recovery assumptions that M46 can benchmark later; it must not depend on M46 being
  complete.
- **Signal gained:** Durability failure-model reasoning and recovery-path engineering.
- **Evidence required:** Crash/recovery validation commands or documented limits, with no claim of
  production durability.

### Scope

- Durability strategy.
- WAL analysis.
- Crash recovery validation.

### Definition of Done

- [ ] Persistence semantics and failure model are documented.
- [ ] Crash/recovery validation is automated where feasible.
- [ ] No claim that the simulator is production durable.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M46 — Recovery benchmarking

- **Branch:** `feat/m46-recovery-benchmarking`
- **PR title:** `perf: benchmark recovery paths`
- **Goal:** Measure recovery objectives from replay and snapshot restoration.
- **Status:** ☐ not started.
- **Dependencies:** M8 replay/recovery, M15–M20 differential fixtures, M45 persistence prototype if
  it lands first, and the benchmark metadata policy from M11/M29.
- **Signal gained:** Recovery objective framing, replay-cost visibility, and benchmark
  interpretation.
- **Evidence required:** Committed recovery benchmark artifacts with hardware/compiler/build/commit
  metadata and dirty-tree state.

### Scope

- Replay performance.
- Snapshot restoration performance.
- Recovery objectives.

### Definition of Done

- [ ] Recovery benchmarks are generated by committed scripts.
- [ ] Results include metadata and dirty-tree state.
- [ ] Docs explain what recovery objective was measured.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M47 — Contiguous order-book storage and cache-locality study

- **Branch:** `feat/m47-contiguous-order-book-storage`
- **PR title:** `perf: study contiguous order-book storage`
- **Goal:** Evaluate flat/contiguous order-book storage alternatives against the existing
  node-based baseline, PMR pooled mode, and intrusive pooled mode.
- **Status:** ☐ not started.
- **Dependencies:** M32 PMR-backed node allocation, PR #112 intrusive `OrderPool` storage, M39
  matching-parameter refactor, M40 engine test consolidation, and M43/M44 locality evidence when
  available.
- **Signal gained:** Cache locality, memory-layout tradeoffs, benchmark interpretation, and replay
  equivalence for alternative storage architectures.
- **Evidence required:** Engine-level benchmark artifacts, replay/differential equivalence, and
  documentation of negative or neutral results.

### Scope

- Flat-array / flat-vector-style storage sketches or prototype path.
- Direct price-index storage study where symbol/price-domain assumptions are explicit.
- Comparison against `std::map`/`std::list`, PMR pooled, and intrusive pooled storage modes.
- Cache-miss/cache-locality analysis using available Linux tooling where supported.
- Replay impact evaluation: identical event streams, final `EngineSnapshot`, and `last_seq`.

### Definition of Done

- [ ] Benchmark artifacts are generated by committed scripts and include metadata.
- [ ] Docs state what allocation/layout changed and what did not.
- [ ] Negative or slower results are documented honestly.
- [ ] No speedup/cache-locality claim without measured evidence.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M48 — DPDK research and prototype

- **Branch:** `feat/m48-dpdk-research-prototype`
- **PR title:** `docs: research DPDK packet-path tradeoffs`
- **Goal:** Late-stage user-space networking investigation only after CPU locality, false sharing,
  storage architecture, Linux profiling, persistence/recovery, and external-review visibility are in
  place.
- **Status:** ☐ not started.
- **Dependencies:** M30/M35 socket evidence, M43 CPU locality, M44 false-sharing/ingress evidence,
  M47 storage locality, M45/M46 persistence/recovery, and issue #94 external-review visibility.
- **Signal gained:** Late-stage networking context and packet-path tradeoff literacy.
- **Evidence required:** Research notes and optional prototype artifacts that clearly separate
  measured behavior from studied design constraints.

### Scope

- DPDK/user-space networking research.
- Packet-path investigation.
- Comparison against kernel networking.

### Non-goals

- No kernel-bypass performance claim without real measurements.
- A weak/fake implementation would add DPDK terminology without environment checks, buildability,
  or measured packet-path evidence.

### Definition of Done

- [ ] Research distinguishes what was measured from what was only studied.
- [ ] Prototype is optional and only if environment support exists.
- [ ] Docs explain why this remains late-stage research rather than core project evidence.
- [ ] `make check` passes; `PROGRESS.md` updated.

---

## M49 — NIC offload and low-latency networking study

- **Branch:** `feat/m49-nic-offload-study`
- **PR title:** `docs: study NIC offload and low-latency networking`
- **Goal:** Research-heavy networking study unless suitable hardware exists.
- **Status:** ☐ not started.
- **Dependencies:** M48 DPDK research, M43 CPU locality, M44 false-sharing/ingress evidence, and
  real NIC/hardware access for any measurement.
- **Signal gained:** Late-stage NIC/offload context, hardware timestamping literacy, and limits of
  offload claims.
- **Evidence required:** Hardware-specific measurements only when real hardware exists; otherwise
  research notes must be labeled as non-measured.

### Scope

- Solarflare.
- Mellanox.
- RSS.
- Timestamping.
- Kernel-bypass ecosystem.

### Definition of Done

- [ ] Docs distinguish research notes from measured artifacts.
- [ ] Hardware-specific measurements are only recorded when real hardware exists.
- [ ] No offload/latency claim without evidence.
- [ ] `make check` passes; `PROGRESS.md` updated.

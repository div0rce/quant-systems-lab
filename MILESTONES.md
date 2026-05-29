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

## Optional M15 — Jane Street application polish

Only after M14, if needed.

- **Branch:** `feat/m15-jane-street-application-polish`
- **PR title:** `docs: Jane Street application positioning and resume bullets`

### Goal

Add recruiter-facing documentation and résumé bullets for SWE and Linux Engineering variants.

### Scope

Add:

```text
docs/recruiting/jane_street_swe.md
docs/recruiting/jane_street_linux.md
```

Include:

1. SWE framing.
2. Linux Engineering framing.
3. conservative résumé bullets.
4. measured benchmark bullets only if M11 produced results.
5. limitations.
6. interview defense notes.

### Definition of Done

- [ ] Docs distinguish SWE vs Linux positioning.
- [ ] No fake metrics.
- [ ] No overclaiming.
- [ ] Resume bullets are conservative and technically defensible.
- [ ] `PROGRESS.md` updated.

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

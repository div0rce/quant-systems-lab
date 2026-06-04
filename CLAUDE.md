# CLAUDE.md — Quant Systems Lab

> **Project memory for Claude Code. Auto-loaded every session. Keep it current.**
> Starting or resuming work? Read this file → then `PROGRESS.md` → then `MILESTONES.md` → then `HANDOFF.md` → then run `/resume`.
> Do not write code until you have read all four and verified the git state.

---

## What Quant Systems Lab is

Quant Systems Lab is a **C++20 exchange-systems portfolio project** built for quant SWE / low-latency systems recruiting.

It implements a deterministic exchange simulator with:

- binary order gateway,
- price-time-priority matching engine,
- deterministic risk checks,
- market-data publisher,
- append-only event log,
- deterministic replay/recovery,
- reproducible latency and throughput benchmarks,
- invariant tests and sanitizer-backed hardening.

This is a portfolio systems project. It is **not** a real exchange. It does **not** connect to real markets. It makes **no** trading, profitability, or production-use claims.

The target reviewer is a quant SWE recruiter or engineer at Jane Street, HRT, Citadel Securities, Citadel, Jump, Optiver, IMC, Two Sigma, or a similar systems-heavy trading firm.

The repo should look like disciplined human engineering, not a tutorial dump, not a fake trading bot, and not a toy README with invented numbers.

---

## Golden rules — non-negotiable

1. **Never commit or push to `main`.** All work happens on a feature branch.
2. **One milestone = one feature branch = one squash-merge PR.**
3. Branch naming convention:
   - Milestone branches use the prefix that describes the work: `feat/mNN-slug`, `test/mNN-slug`,
     `docs/mNN-slug`, `perf/mNN-slug`, or `refactor/mNN-slug`.
   - Examples: `feat/m03-order-book`, `docs/m31-external-review`,
     `refactor/m36-epoll-event-loop-decomposition`.
   - `/start-milestone` must read the selected milestone's exact `Branch:` value from
     `MILESTONES.md`; do not infer a prefix from the milestone number.
4. Use Conventional Commits on feature branches:
   - `chore:`
   - `feat:`
   - `fix:`
   - `test:`
   - `docs:`
   - `refactor:`
   - `perf:`
   - `ci:`
5. A milestone is **DONE** only when:
   - every item in its Definition of Done in `MILESTONES.md` is met,
   - `make check` passes,
   - `PROGRESS.md` is updated,
   - a PR is opened.
6. Claude Code must **never merge its own PR**. The human squash-merges.
7. **Never fabricate benchmark numbers.** Metrics exist only after committed benchmark scripts produce them.
8. No benchmark number enters `README.md`, résumé bullets, PR notes, or docs unless produced by the repo’s benchmark harness.
9. Core matching logic must be deterministic.
10. Core engine logic cannot depend on wall-clock time.
11. Prices are integer ticks. **Never use floating point for price.**
12. Write tests beside deterministic logic.
13. Do not overbuild. Preserve milestone scope.
14. No TODOs in merged PRs. Move follow-ups to the backlog in `MILESTONES.md`.
15. No secrets in git.
16. Qualify progress guarantees precisely: a queue may be wait-free only for the protocol itself and only when payload operations are bounded and non-blocking; do not overstate progress for arbitrary `T`.

---

## Operating model — AI-first, human-in-the-loop

Claude Code does:

- planning,
- branch creation,
- implementation,
- tests,
- docs,
- benchmark harness,
- CI,
- commits,
- PR creation.

The human does:

- reads the PR,
- approves or requests changes,
- squash-merges,
- resumes the next milestone.

Resumability is anchored in:

1. `CLAUDE.md` — canonical Claude Code project memory.
2. `AGENTS.md` — Codex-facing mirror/adapter. Keep synchronized with `CLAUDE.md`.
3. `MILESTONES.md` — ordered roadmap and milestone definitions.
4. `PROGRESS.md` — live state and resume anchor.
5. `HANDOFF.md` — operator manual tying the files together.
6. Git history and PR state.

`AGENTS.md` and `CLAUDE.md` must agree on workflow rules, command lists, roadmap state,
non-overclaiming rules, and benchmark rules.

After interruption, never rely on conversation memory. Reconstruct state from files and git.

---

## Target repo layout

```text
quant-systems-lab/
├── CLAUDE.md
├── AGENTS.md
├── HANDOFF.md
├── MILESTONES.md
├── PROGRESS.md
├── README.md
├── Makefile
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── .gitignore
├── .pre-commit-config.yaml
├── .github/
│   ├── workflows/
│   │   └── ci.yml
│   └── pull_request_template.md
├── .claude/
│   ├── settings.json
│   └── commands/
│       ├── resume.md
│       ├── start-milestone.md
│       ├── finish-milestone.md
│       └── review.md
├── cmake/
│   ├── CompilerWarnings.cmake
│   ├── Sanitizers.cmake
│   └── ProjectOptions.cmake
├── include/
│   └── qsl/
│       ├── core/
│       │   ├── types.hpp
│       │   ├── clock.hpp
│       │   ├── result.hpp
│       │   └── invariants.hpp
│       ├── protocol/
│       │   ├── messages.hpp
│       │   ├── codec.hpp
│       │   └── endian.hpp
│       ├── engine/
│       │   ├── order.hpp
│       │   ├── order_book.hpp
│       │   ├── matching_engine.hpp
│       │   └── risk.hpp
│       ├── feed/
│       │   ├── market_data.hpp
│       │   └── publisher.hpp
│       ├── gateway/
│       │   ├── order_gateway.hpp
│       │   └── session.hpp
│       ├── replay/
│       │   ├── event_log.hpp
│       │   └── recovery.hpp
│       └── util/
│           ├── fixed_string.hpp
│           ├── ring_buffer.hpp
│           └── histogram.hpp
├── src/
│   ├── core/
│   ├── protocol/
│   ├── engine/
│   ├── feed/
│   ├── gateway/
│   ├── replay/
│   └── util/
├── apps/
│   ├── qsl-gateway/         # TCP order gateway
│   ├── qsl-client/          # TCP client CLI
│   ├── qsl-mdfeed/          # UDP market-data feed
│   ├── qsl-loginspect/      # event-log inspector
│   ├── qsl-replay/          # replay/recovery CLI
│   ├── qsl-bench/           # benchmark harness
│   ├── qsl-export-fixture/  # event-log fixture exporter
│   └── qsl-export-stream/   # differential command-stream / shrink exporter
├── ocaml/                   # independent OCaml replay verifier + differential tests
│   ├── dune-project
│   ├── lib/                 # event, parser, invariant, replay_engine, stream_parser
│   ├── bin/                 # verify_replay, replay_snapshot
│   └── test/                # tests + fixtures/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── property/
│   └── CMakeLists.txt
├── benchmarks/
│   ├── bench_order_book.cpp
│   ├── bench_matching_engine.cpp
│   ├── bench_protocol.cpp
│   └── CMakeLists.txt
├── scripts/
│   ├── run_benchmarks.sh
│   ├── generate_synthetic_flow.py
│   ├── check_no_main_commit.sh
│   └── summarize_benchmarks.py
├── data/
│   ├── synthetic/
│   │   └── README.md
│   └── README.md
├── docs/
│   ├── architecture.md
│   ├── binary_protocol.md
│   ├── matching_rules.md
│   ├── replay_and_recovery.md
│   ├── benchmarking.md
│   ├── recruiting_notes.md
│   └── adr/
│       └── 0001-record-architecture-decisions.md
├── results/
│   └── README.md
└── regressions/             # archived minimized differential failures
```

Update this layout if implementation forces a real structural change.

---

## Tech stack

- **Language:** C++20
- **Build:** CMake >= 3.24, Ninja preferred
- **Tests:** Catch2 or GoogleTest
- **Benchmarks:** Google Benchmark or custom benchmark harness
- **Formatting:** clang-format
- **Static analysis:** clang-tidy
- **CI:** GitHub Actions on Ubuntu
- **Dependency approach:** minimal dependencies; use CMake `FetchContent` only if useful

Allowed dependencies:

- Catch2 or GoogleTest
- Google Benchmark
- fmt optional
- spdlog optional

Avoid unnecessary dependencies. A small systems repo with clear primitives beats a dependency graveyard.

---

## Commands

Keep this synchronized with the Makefile.

- `make configure` — configure dev build
- `make build` — build dev preset
- `make test` — run CTest
- `make check` — format check + build + tests
- `make fmt` — apply clang-format
- `make tidy` — clang-tidy target if available
- `make bench` — run benchmark suite
- `make bench-diff` — run differential harness benchmarks
- `make bench-allocator` — run M28 allocator experiment
- `make bench-storage` — run M32 storage experiment
- `make perf-stat` — run Linux `perf stat` workflow where supported
- `make perf-record` — run Linux `perf record/report` workflow where supported
- `make profile-io` — run Linux syscall/socket-path profiling where supported
- `make socket-load` — Linux multi-client TCP connection-scaling load experiment
- `make asan` — build/run sanitizer preset
- `make tsan` — build/run ThreadSanitizer concurrency tests
- `make concurrency-stress` — opt-in repeated concurrency validation loop
- `make socket-stress` — UDP socket-buffer / burst-loss experiment
- `make demo` — run local replay + TCP gateway demo
- `make check-fixtures` — regenerate and verify differential fixtures
- `make check-manifest` — verify fixture provenance manifest
- `make determinism` — verify fixture determinism across compilers where supported
- `make divergence-demo` — exercise the shrinker on an injected divergence
- `make clean` — remove build artifacts

Run `make check` before every PR.

---

## Core design principles

1. Determinism over cleverness.
2. Integer money representation.
3. Explicit serialization.
4. No struct reinterpret-cast protocol hacks.
5. Small interfaces.
6. Tests for invariants.
7. Measured performance only.
8. Honest documentation.
9. Avoid global mutable state.
10. Prefer simple C++ over template acrobatics.
11. Use RAII.
12. Avoid exceptions in hot paths unless explicitly justified.
13. Keep public APIs small.
14. Comments explain **why**, not **what**.
15. For concurrency docs, separate the protocol proof from payload-type assumptions so progress claims stay exact.

---

## Domain model rules

### Symbols

Use compact numeric symbol IDs internally.

```cpp
using SymbolId = std::uint32_t;
```

Synthetic symbols may include:

```text
AAPL
MSFT
NVDA
TSLA
SPY
```

Map external fixed symbol strings to `SymbolId`.

### Price representation

Use integer ticks. Never use floating point for prices.

```cpp
using Price = std::int64_t;
```

Example:

```text
$123.45 with tick scale 100 => 12345
```

### Quantity representation

```cpp
using Quantity = std::uint32_t;
```

### Order ID

```cpp
using OrderId = std::uint64_t;
```

### Sequence number

Every accepted command/event receives a monotonic sequence number.

```cpp
using SeqNo = std::uint64_t;
```

### Timestamp

Use deterministic logical timestamp inside replayable engine paths.

Do not let core matching depend on wall-clock time.

Wall-clock can exist at gateway boundary and benchmark layer, but not in core deterministic engine logic.

---

## Matching rules

Implement a simple price-time-priority limit order book.

### Supported order commands

1. New limit order.
2. New market order.
3. Cancel order.
4. Modify order.

### Side

- Buy
- Sell

### Time in force

Start with:

- GTC
- IOC

Optional later:

- FOK

Do not add advanced order types early. This is where scope dies.

### Matching rules

Buy order matches sell orders when:

```text
buy_price >= best_ask
```

Sell order matches buy orders when:

```text
sell_price <= best_bid
```

Market orders cross against best available resting liquidity until:

1. order fully filled, or
2. book depleted.

### Price-time priority

Within a price level:

1. earlier order has priority,
2. partial fills preserve remaining order priority,
3. modified orders lose priority if price or quantity increases,
4. quantity reduction can preserve priority.

Document this in `docs/matching_rules.md`.

### Engine output events

The matching engine emits:

- `OrderAccepted`
- `OrderRejected`
- `OrderCanceled`
- `OrderModified`
- `Trade`
- `BookUpdate`

The market-data publisher consumes engine events.

---

## Binary protocol rules

Design a compact fixed-width binary protocol first.

### Example message header

```cpp
struct MessageHeader {
    std::uint16_t msg_type;
    std::uint16_t version;
    std::uint32_t body_len;
    std::uint64_t seq_no;
};
```

All encoding/decoding must define:

1. byte order,
2. field widths,
3. message version,
4. maximum message size,
5. rejection behavior for malformed frames.

Use big-endian network byte order at protocol boundary.

Internal structs do not need to match wire layout.

### Client-to-gateway messages

- `NewOrder`
- `CancelOrder`
- `ModifyOrder`
- `Heartbeat`
- `Logout`

### Gateway-to-client messages

- `Ack`
- `Reject`
- `Fill`
- `CancelAck`
- `ModifyAck`
- `HeartbeatAck`

### Market data messages

- `Trade`
- `TopOfBook`
- `BookDelta`
- `Snapshot`

### Required protocol tests

1. encode/decode round trip,
2. malformed header rejection,
3. unsupported version rejection,
4. body length mismatch rejection,
5. unknown message type rejection,
6. deterministic byte fixture test.

No undefined behavior from struct reinterpret casts. Use explicit serialization.

---

## Risk checks

Risk module must be deterministic and explicit.

Initial checks:

1. max order quantity,
2. max notional value,
3. duplicate order ID,
4. unknown symbol,
5. invalid price tick,
6. invalid side,
7. cancel unknown order,
8. modify unknown order.

Optional later:

1. per-client open order cap,
2. per-client gross exposure,
3. kill switch.

Risk must return structured rejection reason codes, such as:

- `RejectReason::UnknownSymbol`
- `RejectReason::MaxQuantityExceeded`
- `RejectReason::DuplicateOrderId`
- `RejectReason::InvalidPrice`
- `RejectReason::UnknownOrder`

---

## Event log and deterministic replay

Every accepted inbound command and emitted engine event must be recordable.

Event log goals:

1. append-only,
2. replayable,
3. deterministic,
4. checksummed if feasible,
5. human-inspectable summary tool.

Minimum log record fields:

- `seq_no`
- `record_type`
- `logical_timestamp`
- `payload_size`
- `payload_bytes`
- checksum optional

Replay invariant:

```text
fresh engine + replay(log) == original engine final state
```

Compare:

1. best bid/ask per symbol,
2. resting order counts,
3. aggregate quantity at each price level,
4. emitted trade sequence,
5. last sequence number.

---

## Gateway and networking staging

Do not start with networking.

Stage 1:

```text
ClientCommand -> Risk -> MatchingEngine -> EngineEvents -> Publisher
```

Stage 2:

TCP gateway:

1. accepts binary client messages,
2. decodes frames,
3. passes commands to in-process gateway,
4. encodes responses.

Stage 3:

Market data publisher:

1. start with in-process subscriber,
2. then add UDP multicast-style simulation or UDP unicast publisher.

Do not overbuild real exchange networking. The goal is credible systems signal, not actual venue certification.

---

## Benchmarking rules

Benchmarks must be reproducible.

Benchmark dimensions:

1. order book add/cancel/modify latency,
2. matching throughput,
3. protocol encode/decode throughput,
4. end-to-end gateway-to-engine latency in-process,
5. replay speed.

Benchmark outputs go to:

```text
results/
```

Required report format:

```text
Hardware:
OS:
Compiler:
Build type:
Git commit:
Dataset:
Scenario:
Metric:
Result:
Date:
```

Synthetic flow scenarios:

1. add-only book build,
2. cancel-heavy flow,
3. crossing aggressive flow,
4. mixed realistic flow,
5. multi-symbol flow.

Use deterministic RNG seed.

---

## Testing expectations

### Unit tests

Test:

1. price levels,
2. order insertion,
3. matching,
4. cancel,
5. modify,
6. protocol codec,
7. risk checks,
8. event log serialization.

### Integration tests

Test full path:

```text
NewOrder -> Risk -> MatchingEngine -> EventLog -> MarketData
```

### Property-style tests

Even without a property-testing library, generate randomized deterministic flows.

Invariants:

1. no negative quantity,
2. no crossed book after matching,
3. total executed quantity cannot exceed submitted quantity,
4. canceled order cannot later trade,
5. rejected order cannot rest,
6. replay final state equals original final state,
7. sequence numbers are strictly increasing,
8. market data sequence follows engine event sequence.

### Sanitizers

Add ASan/UBSan build preset.

ThreadSanitizer only once concurrency exists.

---

## Documentation expectations

Docs must explain:

1. architecture,
2. binary protocol,
3. matching rules,
4. replay/recovery,
5. benchmark methodology,
6. limitations,
7. recruiting positioning.

Avoid phrases:

- production-grade
- institutional-grade
- battle-tested
- high-frequency trading platform
- real trading system
- guaranteed low latency

Use phrases:

- deterministic simulator
- price-time priority
- binary protocol
- append-only event log
- replayable state
- measured benchmarks
- synthetic order flow

---

## Final project bar

This repo succeeds only if a technical reviewer can see:

1. you understand matching semantics,
2. you know prices cannot be floats,
3. you understand deterministic replay,
4. you understand protocol boundaries,
5. you can write C++ without obvious undefined behavior,
6. you can test invariants,
7. you can benchmark without lying,
8. you can manage a clean engineering process.

The project fails if it becomes:

1. a fake trading bot,
2. a Python backtester,
3. a web dashboard first,
4. an overclaimed HFT system,
5. a pile of untested C++,
6. a README with invented numbers.


---

# Jane Street Hong Kong December–February Internship Addendum

This section is additive project context. Do not delete or weaken any earlier Quant Systems Lab instructions. The repo remains a C++20 exchange-systems project. This addendum sharpens the project for Jane Street Software Engineering and Linux Engineering internship applications.

## Target roles from Jane Street Hong Kong

Primary target:

1. **Software Engineer Internship, December–February — Hong Kong**

Secondary target:

2. **Linux Engineer Internship, December–February — Hong Kong**

Lower-priority optional targets:

3. **Strategy and Product Internship, December–February — Hong Kong**
4. **IT Operations Engineer Internship, December–February — Hong Kong**

Do not optimize the repo for IT Operations. IT Ops is a weaker signal for the user's stated goal. The repo should optimize for elite technical software/systems roles.

## Role-fit interpretation

### Jane Street Software Engineering Internship

Relevant job signals:

- top-notch programmer;
- curious, collaborative, eager to learn;
- maintainable, high-quality software intended to reach production;
- exposure to OCaml as primary internal development language;
- some teams use Python;
- work ranges from high-performance trading systems to programming language design.

Project positioning:

- deterministic C++20 exchange simulator;
- binary order gateway;
- price-time-priority matching engine;
- market-data publisher;
- risk checks;
- append-only event log;
- replayable recovery;
- invariant tests;
- reproducible benchmarks;
- **OCaml replay verifier** as targeted Jane Street language/culture signal.

### Jane Street Linux Engineering Internship

Relevant job signals:

- operating system fundamentals;
- computer architecture;
- network protocols;
- command line comfort;
- C, sockets, virtual memory, process lifecycle;
- debugging kernel/performance issues;
- automation;
- production trading infrastructure;
- root-cause analysis;
- firm-wide systems tooling.

Project positioning:

- Linux-focused systems infrastructure simulator;
- TCP order gateway;
- binary protocol framing;
- malformed-frame handling;
- deterministic event logs;
- replay/recovery utilities;
- benchmark automation;
- Linux performance notes;
- socket/process/tooling documentation.

## Additional repo goal: one project, two angles

The same repo must support two résumé framings.

### SWE framing

Project title:

```text
Quant Systems Lab — C++20 Exchange Simulator + OCaml Replay Verifier
```

Resume bullets:

```text
- Built a deterministic C++20 exchange simulator with binary order gateway, price-time-priority matching engine, market-data publisher, risk checks, append-only event log, and replayable recovery path.
- Implemented fixed-width protocol encoding/decoding, deterministic sequencing, and invariant tests covering fills, cancellations, priority preservation, malformed frames, and replay equivalence.
- Added an OCaml replay verifier using immutable state transitions to validate exported engine logs and final book snapshots.
```

### Linux Engineering framing

Project title:

```text
Quant Systems Lab — Linux Systems + Exchange Infrastructure Simulator
```

Resume bullets:

```text
- Built a Linux-focused C++20 exchange infrastructure simulator with TCP order gateway, binary protocol framing, deterministic event logs, replay recovery, and reproducible benchmark tooling.
- Implemented socket-based client/server tooling, malformed-frame handling, append-only log inspection, and recovery utilities for debugging deterministic order-flow scenarios.
- Documented systems tradeoffs across protocol design, process boundaries, replayability, benchmark methodology, and Linux performance measurement.
```

## New required component: OCaml replay verifier

Add an OCaml subproject after the core C++ engine, replay, networking, and benchmark foundations exist. This does not replace the C++ engine. It validates exported event logs and final snapshots using immutable state transitions.

Target layout:

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

OCaml verifier goals:

1. Parse exported C++ event logs or normalized event-log text/JSON fixtures.
2. Represent events as immutable ADTs.
3. Replay immutable state transitions.
4. Verify final book snapshot equivalence.
5. Report invariant violations deterministically.
6. Use Dune for build/test.
7. Keep scope small and defensible.

Why it exists:

Jane Street's SWE program uses OCaml heavily. The project does not need to pretend the user is already an OCaml expert. It should demonstrate that the user can learn the toolchain, model state functionally, and reason about deterministic replay.

## Linux performance documentation requirement

Add Linux-specific notes in docs. These are not fake performance claims. They are documentation of how to measure and reason about performance.

Required docs:

```text
docs/linux_performance.md
docs/socket_gateway.md
```

`docs/linux_performance.md` should eventually cover:

- build mode and compiler flags;
- CPU governor caveats;
- wall-clock vs logical time;
- p50/p95/p99 latency reporting;
- cache effects;
- allocator effects;
- perf basics;
- why benchmark numbers are hardware-dependent;
- what the benchmark does and does not prove.

`docs/socket_gateway.md` should eventually cover:

- frame boundaries;
- partial reads/writes;
- malformed-frame handling;
- disconnect behavior;
- heartbeat behavior;
- why the gateway is intentionally simple.

## Application priority embedded in project decisions

When a tradeoff exists, optimize in this order:

1. Correctness and determinism.
2. Clean C++ systems design.
3. Tests and invariants.
4. Replayability.
5. Protocol clarity.
6. Linux/networking credibility.
7. Measured performance.
8. OCaml functional replay signal.
9. Documentation polish.
10. Aesthetic extras.

Do not build a dashboard before the engine is real. Do not build trading strategies. Do not connect to market data APIs. Do not make profitability claims. The market does not care about decorative software.


## Additive M15–M20 technical roadmap replacing old optional application polish

The prior optional `M15 — Jane Street application polish` milestone is removed. Do not implement recruiter-facing polish as the next milestone. The project should now continue with technical depth:

1. **M15 — Export normalized command streams + final snapshots**
   - Export complete command streams, engine events, rejections, symbol registration order, and final per-symbol snapshots.
   - This gives the OCaml side enough information to replay independently.
2. **M16 — Independent OCaml replay engine**
   - OCaml replays the command stream immutably and computes its own final snapshot.
   - It must not merely inspect the C++ event log.
3. **M17 — Differential replay tests: C++ vs OCaml snapshot equality**
   - CI compares C++ exported snapshots against OCaml-computed snapshots.
4. **M18 — Property-based command generator**
   - Generate seeded randomized command streams covering valid, invalid, duplicate, reused, IOC, market, cancel, modify, and multi-symbol cases.
5. **M19 — Shrinker + minimal failing fixture exporter**
   - Reduce failing generated streams to small, replayable counterexamples.
6. **M20 — Final docs: differential testing architecture**
   - Document the architecture, fixture schemas, property generator, shrinker, and exact limits.

### Strategic reason

The repo should not stop at “built a matching engine.” That is a known portfolio project. The stronger claim is: built a deterministic exchange simulator plus a cross-language differential testing system that can generate, replay, compare, and shrink market-state counterexamples.

### Non-negotiable constraints

- Do not remove existing context or prior milestone history.
- Do not overclaim formal verification.
- Do not claim production exchange behavior.
- Do not claim trading profitability.
- Do not add application-polish docs unless M20 explicitly needs final technical framing.
- Keep C++ as the system under test and OCaml as independent replay/checking infrastructure.
- Every benchmark or performance claim must remain measured by committed scripts.


---

## Phase III / Phase IV roadmap: concurrency, memory, Linux profiling, and external review

v0.1.0 established the correctness-first exchange-systems lab: deterministic matching,
replay/recovery, binary protocol handling, OCaml differential testing, property
generation/shrinking, sanitizer hardening, and release hygiene.

The next arc is not more product surface area. It is systems credibility.

Phase III focuses on:
1. bounded SPSC queue internals;
2. memory-ordering documentation and stress tests;
3. a threaded gateway-engine-feed prototype;
4. ThreadSanitizer coverage;
5. allocator/memory-pool experiments.

Phase IV focuses on:
1. Linux perf workflow and honest constrained-environment validation;
2. full hardware PMU evidence when a PMU-capable Linux host is available (issue #90);
3. kernel/socket path profiling;
4. Linux socket hardening experiments;
5. an external review/maintainer-signal package;
6. pool-backed order-book storage integration;
7. advanced concurrency validation.

Do not add dashboards, strategies, market-data APIs, FIX adapters, Docker packaging, or
aesthetic product work before M24–M48 unless the human explicitly changes priorities.

The correct claim after this arc is:

> "correctness-first deterministic exchange-systems lab with measured concurrency, allocator,
> constrained Linux perf workflow, and socket-profiling evidence."

Do not claim real hardware PMU evidence until issue #90 is completed on a bare-metal or
PMU-capable Linux target. Current M29 artifacts are constrained-environment validation only.

The incorrect claims remain forbidden:

- production HFT platform
- real exchange
- formally verified exchange
- profitable trading system
- guaranteed low latency
- production networking stack

`make tsan` (ThreadSanitizer) exists after M27 and runs the concurrency-labelled tests where the
toolchain supports TSan.

After M27 exists, ThreadSanitizer remains dynamic-analysis evidence, not a correctness proof. TSan
validates executed schedules and can detect races that occur during tested executions; it does not
prove correctness across all possible thread interleavings.

M28 allocator evidence is allocator evidence only. M32 delivered a scoped PMR-backed order-book
node-allocation experiment measured on engine-level workloads. Direct intrusive
`OrderPool<Capacity>` order-book storage remains future work tracked separately by issue #95.

## Current post-M35 roadmap memory

Current landed state on `main`: M35 (PR #100, a86b701) and the project-memory syncs (PR #101
40f9249, PR #102 7092423) are merged. A CodeScene Code Health analysis then inserted a
repository-health refactor phase: milestones **M36–M42** (M36 epoll decomposition, M37
threaded-pipeline stage helpers, M38 shrinker passes, M39 order-book matching parameters, M40 engine
test-suite consolidation, M41 session frame dispatch, M42 shared shell-script helpers) — all
behavior-preserving.

Original roadmap after M35 shifted +7 to **M43–M48**: M43 NUMA awareness study; M44 lock-free
ingress pipeline (not lock-free matching); M45 exchange-grade persistence prototype; M46 recovery
benchmarking; M47 DPDK research/prototype; M48 NIC offload and low-latency networking study. NUMA is
now M43; do not start it until the inserted M36–M42 refactors are done or the human reprioritizes.

Issue #90 remains the full hardware-PMU evidence debt. Issue #99 tracks broader
byte-budgeted/streaming gateway response generation. Issue #95 tracks intrusive/custom-node
`OrderPool<Capacity>` order-book storage. Issue #94 is the external technical review request.

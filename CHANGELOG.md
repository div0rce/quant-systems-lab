# Changelog

All notable changes to this project. The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/). The first tagged release is v0.1.0.

## [Unreleased]

_Nothing yet._

## [0.2.2] - 2026-06-25

A security/robustness **hardening** wave plus two measured order-book **performance** wins (driven by
a multi-round adversarial bug hunt, converged 5→2→1→0 confirmed bugs, and flamegraph-guided
optimization), then a full **documentation overhaul**: a reproducible performance-evidence report, a
rebuilt README, mermaid diagrams across the docs, and a repo-wide style sweep. Same honesty bar: a
deterministic C++20 exchange simulator and cross-language differential-testing harness, **not** a
production exchange, no real-market connectivity, no latency or profitability claims, not formal
verification. Determinism preserved throughout (fixtures byte-identical across g++/clang++ and vs the
committed copies; the OCaml differential passes). `make check`/`make asan` 272/272 (the latter a real
UBSan abort gate).

### Fixed

- **Reject out-of-domain enum bytes in the decoders (#136).** `replay::decode_command` (NewLimit /
  NewMarket) and `protocol::decode_reject` cast raw bytes to `Side` / `TimeInForce` / `RejectReason`
  without validating the domain. Since the replay path applies decoded commands straight to the
  engine with no gateway risk check, a corrupt log record could silently diverge replayed state.
  Both now validate via `core::is_valid` (added `is_valid(RejectReason)`) and refuse out-of-domain
  bytes like a malformed frame.
- **Network-path hardening (#137, #140, #143).** The TCP gateway now retries `EINTR` in its
  read/write paths and survives transient `accept()` errors (`EINTR`/`ECONNABORTED`) instead of
  tearing the listener down; both the threaded acceptor (back-off retry) and the epoll loop (listener
  disarm/re-arm) survive fd exhaustion (`EMFILE`/`ENFILE`); a `TcpServerOptions::max_active_connections`
  cap sheds load; the epoll loop bounds accepts per tick for fairness; and `UdpPublisher` counts
  `send_failures` rather than silently dropping datagrams.
- **CLI argument validation (#141).** `qsl-client`, `qsl-mdfeed`, and `qsl-export-fixture` parse
  numeric arguments with `std::from_chars` and reject malformed / out-of-range input with a usage
  message and non-zero exit, instead of `std::terminate` (from an uncaught `std::sto*` exception) or
  silently truncating an out-of-range port.
- **UBSan gate now actually fails (#142).** The `asan` preset adds `-fno-sanitize-recover=undefined`
  so UBSan **aborts** on the first violation. It previously ran in recover mode (print a diagnostic,
  exit 0), so a pure-UBSan defect passed `make asan` / CI green. The tree is UBSan-clean under the
  strict gate.
- **OCaml `diff_report` robustness (#144).** The differential-bundle bin guards each fixture
  (catching `Stream_parser.Parse_error` / `Sys_error`) so one malformed or unreadable fixture cannot
  abort the whole batch and silently lose the divergence bundles for the rest.

### Performance

- **`try_emplace` for baseline price levels (#138).** `OrderBook::level_for` used
  `std::map::emplace`, which allocates and frees a node even when the price level already exists.
  `try_emplace` avoids that on the steady-state common path. Measured back-to-back A/B on the
  `qsl-bench profile` workload: **~+5%**.
- **Order-index hash load-factor cap (#145).** The `OrderId → Locator` index is the busiest structure
  on the engine hot path (1-4 point lookups per op). Capping its `max_load_factor` at 0.25 shortens
  probe chains. Measured A/B: **~+18.6%**. Determinism is unaffected, the index is never iterated
  for output.
- **Flamegraph regenerated (#135, #139, #146)** against the new code, now a dense (~20k-sample),
  fully-symbolized frame-pointer profile with zero `[unknown]` frames.

### Documentation and evidence

- **Performance-evidence report (#148, #150).** New `PERFORMANCE.md` profiles the matching-engine hot
  path with Linux `perf` and flamegraphs on ARM64 (Apple M2), comparing the **v0.1.0 first release to
  v0.2.2** (the same `qsl-perfeval` harness ported into a `v0.1.0` worktree, measured on the same
  host): allocations/order 4.094 → 2.670 (-35%), cycles/order 310.7 → 289.5 (-6.8%), branch-miss rate
  2.01% → 1.68%, latency unchanged. Cache-miss rate is reported unavailable, never estimated (Apple
  Silicon PMU; #90). A dedicated `qsl-perfeval` target (plus a `qsl-perfeval-allocs` counting build)
  makes every number reproducible. The before/after flamegraphs render fully symbolized with zero
  `[unknown]` frames; the unresolvable boundary frames were identified (an fp glibc-malloc-boundary
  artifact and the vDSO `clock_gettime` leaf) and folded into their resolved caller, not hidden.
- **Documentation overhaul and README rebuild (#147, #149).** Every doc, artifact, and provenance
  header was refreshed to the v0.2.2 state, and the README was rebuilt to lead with the performance
  numbers and the matching-engine flamegraph. Mermaid diagrams were added across the docs (matching
  rules, binary protocol, persistence, concurrency model, memory ordering, gateway accept loop, OCaml
  differential), and every em dash and en dash was removed repo-wide.
- **Honesty corrections, made in the open.** Two measurement errors caught by self-review and code
  review were fixed and documented rather than buried: the allocation counter had missed
  over-aligned allocations (so the cumulative reduction is -35%, not the -73% an earlier draft
  claimed), and a thermal-warmup p99 artifact was corrected to "latency distribution unchanged".
- **Previously-unaddressed review findings (#150).** Acted on CodeRabbit comments left open on earlier
  PRs: a non-finite `strtod` guard in the bench profile timer, `ENOPROTOOPT`/`EOPNOTSUPP` added to the
  threaded accept-retry set to match the epoll path, and the perfeval harness's resting-order
  tracking, percentile index, and argument validation.

## [0.2.1] - 2026-06-21

Two backlog items, reprioritized by the maintainer and delivered, plus a resume-anchor and
perf-evidence consistency sweep. Same honesty bar as prior releases: a deterministic C++20 exchange
simulator and cross-language differential-testing harness, **not** a production exchange, no
real-market connectivity, no latency or profitability claims, and not formal verification.

### Added

- **FIX-like text protocol adapter (#29).** A human-readable `tag=value` (SOH-framed) codec
  (`include/qsl/protocol/fix.hpp`, `src/protocol/fix.cpp`) over the **same internal message structs**
  as the binary codec, with genuine FIX framing. BeginString (8) / BodyLength (9) / MsgType (35) /
  … / mod-256 CheckSum (10), for the client→gateway order path: NewOrderSingle (`35=D`) → `NewOrder`
  and OrderCancelRequest (`35=F`) → `CancelOrder`. Decoding is total, deterministic, and `noexcept`
  (fixed field table, `std::from_chars`, `std::string_view`; no heap on the decode path) and reports
  every malformed input through a `FixError` taxonomy mirroring the binary codec's `DecodeError`.
  Covered by `tests/unit/test_fix_protocol.cpp`, including a **cross-codec equivalence** test (binary
  and FIX decode the same order to identical structs) and a byte-pinned fixture; documented in
  `docs/fix_protocol.md`. Prices stay integer ticks and Symbol carries the numeric `SymbolId`
  (documented simplifications, never floating-point price).
- **`make flamegraph` (#32).** Renders a Linux `perf` call-graph flamegraph
  (`results/flamegraph.svg` + a provenance/classification `results/flamegraph.txt`) from the
  benchmark harness via `scripts/flamegraph.py`, a dependency-free (stdlib-only) stackcollapse + SVG
  renderer (deterministic; unit-tested in `tests/shell/test_flamegraph.sh`), so the artifact is
  reproducible from the repo without vendoring the Perl FlameGraph toolkit. The committed artifact is
  a software cpu-clock sampling **hot-symbol profile** from the bare-metal Fedora Asahi host, not a
  latency/throughput claim; full hardware cache-PMU evidence stays in issue #90.

### Changed

- Synced the `/resume` anchors and perf-evidence wording to the released `v0.2.0` state and narrowed
  an overstated Apple **Blizzard** (E-core) PMU claim, those rows read `<not counted>` because the
  single-threaded benchmark stays on the Avalanche P-cores (Codex follow-up to PRs #127/#128):
  `PROGRESS.md`, `AGENTS.md`/`CLAUDE.md` agreement, and `docs/perf_analysis.md`.

## [0.2.0] - 2026-06-21

Quant Systems Lab v0.2.0, the Phase III/IV systems arc (M24-M49: a bounded SPSC queue and threaded
pipeline, ThreadSanitizer coverage, allocator and order-book storage experiments, Linux
perf/socket/NUMA studies, an epoll gateway prototype, event-log persistence/recovery, and DPDK/NIC
research) plus a **bare-metal Linux evidence refresh**. Same honesty bar as v0.1.0: a deterministic
C++20 exchange simulator and cross-language differential-testing harness, **not** a production
exchange, no real-market connectivity, no latency or profitability claims, and not formal
verification.

### Added

- v0.2.0 evidence refresh: regenerated every `results/*.txt` on a **bare-metal** Apple M2 (aarch64)
  Fedora Asahi Linux host (`systemd-detect-virt: none`). `scripts/perf_stat.sh` now classifies three
  ways, full `hardware PMU evidence`, `partial hardware PMU evidence` (real counters, incomplete
  set), or `constrained-environment validation (no hardware PMU access)`, so the perf artifact is
  honestly labeled **partial hardware PMU evidence**: real `cycles`/`instructions`/`branches`/
  `branch-misses` off the Apple Avalanche/Blizzard PMUs, with `cache-references`/`cache-misses`
  unsupported by the Apple Silicon PMU. Issue #90 is narrowed to the cache-counter set (needs a PMU
  microarchitecture that exposes it; bare metal alone is not enough).
- `qsl_publish_artifact` redacts **every** non-broadcast MAC address (link/ether, permaddr,
  bridge_id/designated_root, group_address, and the `wlx<mac>` altname) at publish time, so
  generated evidence cannot leak host hardware identifiers.
- `tests/shell/test_qsl_common.sh` (registered with CTest, runs under `make check`) covering the MAC
  sanitization and the trailing-whitespace/blank-line trimming of the shared artifact-publish
  helper.
- M42: `scripts/qsl_common.sh`, shared by the socket/perf shell workflows for repo-relative
  dirty-tree exclusions, metadata helpers, Linux guards, TCP readiness probes, and process-stop
  handling.
- Added `FixtureExportRequest` / `FixtureExportMode` and `write_fixture_export` so the replay
  fixture library owns qsl-export-stream export orchestration while the CLI stays argument parsing
  only.
- Added `TcpServerOptions::max_response_bytes` so the portable TCP transport uses the bounded
  session path and can reject high-fanout response generation before gateway mutation.
- Added `OrderBook::Storage::IntrusivePooled`, an opt-in storage mode that backs resting
  `engine::Order` objects with the M28 raw `OrderPool` and custom FIFO order nodes while preserving
  baseline and PMR-backed storage modes.
- Added `RejectReason::StorageExhausted` so the gateway can reject an opt-in intrusive-storage
  allocation limit before mutating the matching engine.
- Added a stateful market-like synthetic flow generator v2 with multi-symbol skew, drifting
  reference prices, GTC/IOC limits, market orders, active cancel/modify targets, stale commands, and
  regenerated OCaml stream fixtures.
- Added portable threaded `TcpServer` serving: accepted connections run in per-connection workers
  while gateway mutation remains serialized. `TcpServerOptions` now exposes `listen_backlog` and a
  `max_accepts` test/embedding hook.
- M35: `scripts/socket_load.sh` (`make socket-load`, Linux-only), multi-client TCP
  connection-scaling load coverage comparing the portable threaded TCP gateway and the epoll gateway under
  bounded loopback pressure, with constrained metadata in `results/socket_load_summary.txt`.
- M43: `scripts/numa_affinity_study.sh` (`make numa-study`, Linux-only). CPU-affinity /
  scheduler-migration / NUMA-locality study tooling with explicit `full-linux-numa`,
  `linux-constrained`, or `unsupported-host` evidence classification.
- M44: `scripts/run_false_sharing_study.sh` (`make false-sharing-study`), benchmark-only
  packed-vs-padded SPSC queue-cursor contention study with metadata in
  `results/false_sharing_study.txt`.
- M47: `OrderBook::Storage::Contiguous`, an opt-in fixed-band direct price-indexed storage mode
  that uses occupancy bitmaps and contiguous per-level FIFO vectors for bounded-domain
  cache/locality study work.
- M34: Linux-only `EpollServer` gateway transport prototype. It uses one `epoll` loop,
  nonblocking accept/read/write, per-client outbound buffers, and one deterministic `Session` per
  client; `qsl-gateway <port> --epoll` opts in on Linux.
- M34: epoll gateway tests cover platform scoping, invalid bind-host rejection, and two
  simultaneous loopback clients handled by one event loop, plus soft-backpressure and hard-cap
  response-budget cases, including disconnect-after-write draining and queued-reply preservation
  before a later over-cap close.
- M33: deterministic pipeline scheduling perturbation (`PipelinePerturbation`) so concurrency tests
  exercise different input/engine/output pacing patterns without timing-sensitive sleeps.
- M33: `make concurrency-stress` / `scripts/concurrency_stress.sh`, an opt-in repeated
  concurrency-test loop for longer local or Linux/TSan runs outside normal CI.
- M32: `OrderBook::Storage::{Baseline,Pooled}` and `MatchingEngine(OrderBook::Storage)` for a
  scoped PMR-backed order-book node-allocation experiment. `Storage::Pooled` routes per-book
  `std::list`, `std::map`, and `std::unordered_map` node allocation through
  `std::pmr::unsynchronized_pool_resource`; baseline storage remains the default.
- M32: `make bench-storage` / `scripts/run_storage_benchmarks.sh` / `qsl-bench storage`, an
  engine-level benchmark comparing baseline order-book storage against PMR pooled node allocation.
- M47: `make bench-storage` now also compares the contiguous direct-price-indexed mode against
  baseline, PMR pooled, and intrusive pooled storage modes. Results remain hardware/build
  dependent and are not a speedup or production-latency claim.
- PR #112: the later #95 follow-up adds the separate intrusive/custom-node `OrderPool<Capacity>`
  integration path that M32 intentionally kept out of its PMR scope.
- M30: optional UDP receive-buffer (`SO_RCVBUF`) sizing on the market-data client, with the
  granted size read back via `getsockopt`. `qsl-mdfeed subscribe` gains a `[rcvbuf_bytes]`
  argument and `qsl-mdfeed publish` an `[orders]` burst-size argument.
- M30: `scripts/profile_gateway_io.sh` (`make profile-io`, Linux-only), profiles the gateway
  syscall / kernel-socket path with `strace -f -c` plus procfs rusage (`/proc/<pid>/{stat,status}`),
  distinguishing user-space matching cost from kernel/socket overhead.
- M30: `scripts/socket_stress.sh` (`make socket-stress`, portable). UDP burst/gap and
  receive-socket-buffer experiment over loopback, run over multiple trials.

### Changed

- M42: `qsl-export-stream` now reports clean usage errors for missing or invalid numeric CLI
  arguments instead of terminating from uncaught parse exceptions.
- The default portable TCP gateway path now accepts multiple clients concurrently via threaded
  connection workers. It remains a simple portable socket transport, not the Linux epoll path.
- M39: encapsulated order-book matching parameters into private match/query contexts and extracted
  fill, erase, and level-lookup helpers. Public order-book behavior, deterministic matching output,
  integer-tick prices, and wall-clock-independent engine semantics are unchanged.
- M39: renamed replay `RecordType` enumerators to `CommandRecord` / `EventRecord` while preserving
  numeric log values, avoiding `-Wshadow` ambiguity with `qsl::replay::Command`.
- M38: split the command-stream shrinker into named reduction/remap passes. `shrink` and
  `renumber` preserve deterministic output and public behavior while moving contiguous removal,
  field simplification, symbol remapping, and order-id compaction into small helpers.
- M37: decomposed the threaded pipeline into a run context plus named input/engine/output stage
  helpers, and consolidated concurrency-test assertion/producer-consumer helpers. Public
  CLI/protocol behavior, deterministic pipeline results, and benchmark claims are unchanged.
- M37: shared `PipelineProbe` counters remain usable across repeated runs; returned
  `PipelineResult` backpressure counters now report the per-run delta rather than cumulative probe
  totals.

### Documentation

- v0.2.0 documentation sweep: reframed the perf evidence from "constrained Docker validation" to
  **bare-metal partial hardware PMU evidence** across `docs/perf_analysis.md`,
  `docs/linux_performance.md`, ADR 0007, `CLAUDE.md`, `results/README.md`, `docs/recruiting_notes.md`,
  and the README Limitations section; corrected stale Docker/containerized framing in
  `docs/socket_profiling.md` and `docs/pool_backed_storage.md` (artifacts are now bare-metal Apple
  M2 runs); rewrote `docs/release_readiness.md` for the M0-M49 + v0.2.0 state (241 tests, six CI
  jobs); added storage/epoll/durability components to `docs/architecture.md`; and clarified that the
  independent OCaml replay engine (M16) re-implements matching while the M14 log verifier does not.
- Synchronized project-memory files before repository-health planning (PR #101, PR #102): M35 is
  merged as PR #100; the project-memory syncs landed; the repository-health analysis has since been
  completed (see the inserted refactor phase below).
- Inserted a repository-health refactor phase after M35, derived from a CodeScene Code Health
  analysis (5 production + 6 test files below 9.0) plus one manually-identified shell-maintainability
  milestone: seven refactor milestones M36-M42 (epoll decomposition; threaded-pipeline stage helpers;
  shrinker passes; order-book matching parameters; engine test-suite consolidation; session frame
  dispatch; shared shell-script helpers). The original networking/persistence roadmap was shifted
  after those refactors; the later systems-roadmap audit extends future scope to M43-M49.
- Updated the future systems-engineering roadmap after PR #112: M43 now covers NUMA plus CPU
  affinity, scheduler migration, and locality caveats; M44 is expanded in place for ingress
  memory-ordering and false-sharing evidence; M47 is inserted for contiguous order-book
  storage/cache-locality; DPDK and NIC offload shift to M48/M49 as late-stage research. Completed
  milestone history and merged PR references are unchanged.
- M34: updated socket-gateway docs and added ADR 0010 to distinguish the Linux epoll architecture
  prototype from M35 multi-client load/socket-pressure evidence. The docs cover EAGAIN/EWOULDBLOCK,
  partial writes, half-close flushing, and bounded outbound buffering.
- M33: concurrency docs now distinguish static happens-before reasoning, TSan, deterministic
  schedule perturbation, and repeated stress as evidence over executed schedules rather than proof
  over all interleavings.
- M32: added `docs/pool_backed_storage.md` and ADR 0009 to distinguish M28 raw-object pool
  mechanics, M32 PMR container-node allocation, and the later intrusive/custom-node order-book
  redesign. Added `results/pool_backed_storage.txt` as the measured engine-level artifact.
- M47: expanded `docs/pool_backed_storage.md` and `results/README.md` to describe the contiguous
  direct-price-indexed storage mode, its fixed price-domain assumption, affected allocations, and
  interpretation limits.
- M31: added an external-review package, `docs/review_request.md` (an adversarial review
  checklist over SPSC memory ordering, backpressure, threaded ownership, event-log integrity under
  concurrency, and benchmark/profiling + Linux/socket methodology), `docs/review_feedback.md` (an
  honest, auditable feedback record stating no external review has occurred yet, no fabricated
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
- Expanded the post-M29 roadmap through M49, covering pool-backed and contiguous order-book
  storage, advanced concurrency validation, epoll gateway work, multi-client socket pressure, NUMA
  and CPU-affinity studies, ingress memory-ordering/false-sharing validation, stronger persistence,
  recovery benchmarking, late-stage DPDK research, and NIC offload study.
- Added explicit backlog distinctions: TSan coverage is dynamic-analysis evidence rather than
  proof, and M28 allocator results are allocator evidence rather than engine-storage evidence.

## [0.1.0] - 2026-05-31

Quant Systems Lab v0.1.0, a deterministic C++20 exchange simulator and cross-language
differential-testing harness, built as a systems-engineering portfolio project. It is **not** a
production exchange, is not connected to real markets, and makes no latency or profitability
claims; the cross-language differential layer is property-based testing, **not** formal
verification. Benchmarks are synthetic microbenchmarks recorded in `results/` and are
hardware/compiler/build-dependent.

### Post-M22 backlog hardening (GitHub issues #34, #51)

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

### Phase II, cross-language differential testing (M15-M20)

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

### Core simulator and tooling (M3-M14)

- OCaml replay verifier checking exported event logs against replay invariants (M14).
- Final architecture/demo/recruiting documentation and `make demo` (M13).
- Hardening: ASan/UBSan, randomized invariant tests, and structure-aware protocol fuzzing in CI
  (M12).
- Reproducible benchmark harness writing `results/latest.txt` with full metadata (M11).
- Append-only event log and deterministic replay/recovery (M7-M8).
- Price-time-priority matching engine, deterministic risk checks, and a market-data publisher
  (M3-M6).

(Networking: a loopback TCP order gateway (M9) and UDP market-data feed (M10), local,
unauthenticated; see `SECURITY.md`.)

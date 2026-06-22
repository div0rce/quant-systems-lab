# Results

Benchmark results produced by `make bench` and scripts under `scripts/`.

- `latest.txt` — core microbenchmarks (`make bench`, `apps/qsl-bench`).
- `differential.txt` — differential-testing harness benchmarks: command-stream generation,
  gateway replay, and shrinking (`make bench-diff`, `qsl-bench diff`).
- `allocator_experiment.txt` — isolated M28 allocation experiment comparing `engine::Order`
  `new/delete` with fixed-pool acquire/release (`make bench-allocator`, `qsl-bench pool`).
- `pool_backed_storage.txt` — engine-level storage experiment comparing baseline order-book node
  allocation against PMR-backed `std::list`/`std::map`/`std::unordered_map` node allocation, the
  opt-in intrusive `OrderPool`-backed resting-order storage mode, and the M47 fixed-band
  contiguous direct-price-indexed storage mode (`make bench-storage`, `qsl-bench storage`). Treat
  the contiguous row as a bounded-domain cache/locality study, not general sparse-price-book
  evidence.
- `perf_stat_linux.txt` — Linux `perf stat` output for the benchmark harness (`make perf-stat`).
  Its `Artifact:` field classifies the run three ways: `hardware PMU evidence` (full — every
  requested counter including cache events captured), `partial hardware PMU evidence` (real
  hardware counters present but the requested set is incomplete — the **current** bare-metal Apple
  Silicon state, where `cache-references`/`cache-misses` are `<not supported>`), or
  `constrained-environment validation (no hardware PMU access)` (no hardware counter at all). Only
  the first is full evidence; issue #90 tracks the missing cache-counter set.
- `perf_report_linux.txt` — Linux `perf record/report` hot-symbol output for the benchmark
  harness (`make perf-record`). It is useful as a hot-symbol profile only when the file says
  `No samples: no`, `Insufficient samples: no`, and the sample count meets the reported minimum.
- `flamegraph.svg` / `flamegraph.txt` — Linux `perf` call-graph flamegraph (`make flamegraph`,
  issue #32) rendered by the dependency-free `scripts/flamegraph.py`. The `.svg` is the visual
  (frame width ∝ on-CPU samples) with provenance in a leading XML comment; the `.txt` carries
  provenance, the `Artifact:` classification, and the top folded stacks. It is a software cpu-clock
  sampling profile for hot-symbol investigation, not a latency/throughput claim — trust frame widths
  only when the `.txt` reports a `flamegraph (...)` artifact with enough samples.
- `numa_affinity_study.txt` — Linux CPU-affinity / scheduler-migration / NUMA-locality study
  output (`make numa-study`). It must self-classify as `full-linux-numa`, `linux-constrained`, or
  `unsupported-host`; only `full-linux-numa` is full NUMA evidence.
- `false_sharing_study.txt` — benchmark-only packed-vs-padded SPSC queue-cursor contention study
  (`make false-sharing-study`). It is research-note evidence about cache-line sharing shape, not
  a production throughput or latency claim.
- `crash_recovery_validation.txt` — M45 SIGKILL crash / torn-tail recovery validation for the
  append-only event log across durability modes (`make crash-recovery`). It is process-kill
  evidence only: it validates crash-mid-append recovery and acknowledged-record retention across
  process death, not power-loss or OS-crash durability (see `docs/persistence.md`).
- `recovery_benchmarks.txt` — M46 recovery benchmarking (`make bench-recovery`,
  `qsl-bench recovery`): full-replay restart cost (log read/verify/classify plus replay into a
  fresh engine) at several log lengths, against a benchmark-only in-memory snapshot-restoration
  prototype at several live-state depths. Restart cost on this host (RTO-style) only; no
  production recovery-time claim (see `docs/replay_and_recovery.md`).
- `dpdk_environment.txt` — M48 non-mutating DPDK environment support check (`make dpdk-check`).
  It records whether the host can build or potentially run a DPDK prototype, but it never binds
  devices, reserves hugepages, sends packets, or supports a kernel-bypass performance claim.
- `nic_offload_environment.txt` — M49 non-mutating NIC offload/timestamping capability check
  (`make nic-offload-check`). It records read-only host and interface context where Linux NICs are
  visible, but it never changes offload flags, RSS tables, timestamp filters, driver bindings, IRQ
  placement, or packet traffic, and it does not support a latency claim.

## Policy

- No results are committed until produced by the benchmark harness (M11).
- Results include hardware, compiler, build type, and provenance metadata. New or migrated
  artifact generators should use:
  - `Provenance version: 1`
  - `Git commit (informational): ...`
  - `Source digest: sha256:...`
  - `Source digest scope: ...`
  - `Dirty inputs: no|yes`
  - `Generated output: ...`
- No estimated or fabricated numbers. They are synthetic microbenchmarks, hardware/compiler/
  build-dependent — not production throughput.
- For migrated artifacts, `Source digest` is the authoritative provenance identity. `Git commit
  (informational)` may change after rebase or squash merge; reviewers should treat a source-digest
  mismatch or `Dirty inputs: yes` as stale evidence, not commit-hash drift alone.
- Migrated generators must not emit `Source commit:` or `Generated from commit:`. Existing
  historical artifacts may remain on the older schema only until their generator is deliberately
  migrated and the corresponding artifact is regenerated.
- The M45A/M45B provenance migration converts the active NUMA and false-sharing artifacts first,
  then the perf, socket, allocator, storage, differential, and core benchmark artifacts. Future
  artifact generators should use the same source-digest schema from the start.
- Future storage, CPU-affinity, false-sharing, DPDK, or NIC artifacts must state whether they are
  full hardware evidence, constrained-environment validation, or research notes only.
- CPU-affinity and NUMA artifacts must include the chosen CPU, whether `taskset` and `perf` ran,
  whether unpinned and pinned migration/context-switch counters were captured, whether `numactl`
  topology was available, whether node-local/remote NUMA binding succeeded, and a caveat line.
- DPDK artifacts must state whether they are build-only, virtual-device, loopback-like, real-NIC,
  or unsupported-host evidence, and must record hugepage/device-binding state before any packet
  path result is interpreted.
- NIC offload artifacts must state whether they are unsupported-host, read-only capability
  observation, or measured real-NIC evidence. Any measured artifact must record device, driver,
  firmware, offload state, RSS/queue state, timestamp source, CPU/IRQ placement, packet workload,
  drop/backpressure behavior, and provenance before supporting any offload or latency statement.

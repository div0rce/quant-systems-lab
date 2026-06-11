# Results

Benchmark results produced by `make bench` and scripts under `scripts/`.

- `latest.txt` — core microbenchmarks (`make bench`, `apps/qsl-bench`).
- `differential.txt` — differential-testing harness benchmarks: command-stream generation,
  gateway replay, and shrinking (`make bench-diff`, `qsl-bench diff`).
- `allocator_experiment.txt` — isolated M28 allocation experiment comparing `engine::Order`
  `new/delete` with fixed-pool acquire/release (`make bench-allocator`, `qsl-bench pool`).
- `pool_backed_storage.txt` — engine-level storage experiment comparing baseline order-book node
  allocation against PMR-backed `std::list`/`std::map`/`std::unordered_map` node allocation and the
  opt-in intrusive `OrderPool`-backed resting-order storage mode (`make bench-storage`,
  `qsl-bench storage`).
  Future M47 contiguous-storage artifacts should make the compared layouts explicit and must not
  be described as cache-locality evidence unless generated from the corresponding code and tooling.
- `perf_stat_linux.txt` — Linux `perf stat` output for the benchmark harness (`make perf-stat`).
  It is full hardware-counter evidence only when the file says `Artifact: hardware PMU evidence`
  and `Unsupported counters detected: no`; otherwise it is constrained-environment validation.
- `perf_report_linux.txt` — Linux `perf record/report` hot-symbol output for the benchmark
  harness (`make perf-record`). It is useful as a hot-symbol profile only when the file says
  `No samples: no`, `Insufficient samples: no`, and the sample count meets the reported minimum.
- `numa_affinity_study.txt` — Linux CPU-affinity / scheduler-migration / NUMA-locality study
  output (`make numa-study`). It must self-classify as `full-linux-numa`, `linux-constrained`, or
  `unsupported-host`; only `full-linux-numa` is full NUMA evidence.
- `false_sharing_study.txt` — benchmark-only packed-vs-padded SPSC queue-cursor contention study
  (`make false-sharing-study`). It is research-note evidence about cache-line sharing shape, not
  a production throughput or latency claim.

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
  historical artifacts may remain on the older schema until a deliberate migration PR converts their
  generators and regenerates them.
- M45A converts only the currently active provenance pain points: `numa_affinity_study.txt` and
  `false_sharing_study.txt`. A follow-up migration should convert the perf, socket, allocator,
  storage, and core benchmark artifacts once the schema is proven.
- Future storage, CPU-affinity, false-sharing, DPDK, or NIC artifacts must state whether they are
  full hardware evidence, constrained-environment validation, or research notes only.
- CPU-affinity and NUMA artifacts must include the chosen CPU, whether `taskset` and `perf` ran,
  whether unpinned and pinned migration/context-switch counters were captured, whether `numactl`
  topology was available, whether node-local/remote NUMA binding succeeded, and a caveat line.

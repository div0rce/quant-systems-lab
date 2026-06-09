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

## Policy

- No results are committed until produced by the benchmark harness (M11).
- Results include hardware, compiler, build type, and git commit.
- No estimated or fabricated numbers. They are synthetic microbenchmarks, hardware/compiler/
  build-dependent — not production throughput.
- Future storage, CPU-affinity, false-sharing, DPDK, or NIC artifacts must state whether they are
  full hardware evidence, constrained-environment validation, or research notes only.
- CPU-affinity and NUMA artifacts must include the chosen CPU, whether `taskset` and `perf` ran,
  whether unpinned and pinned migration/context-switch counters were captured, whether `numactl`
  topology was available, whether node-local/remote NUMA binding succeeded, and a caveat line.

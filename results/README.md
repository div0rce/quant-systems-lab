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
- `perf_stat_linux.txt` — Linux `perf stat` output for the benchmark harness (`make perf-stat`).
  It is full hardware-counter evidence only when the file says `Artifact: hardware PMU evidence`
  and `Unsupported counters detected: no`; otherwise it is constrained-environment validation.
- `perf_report_linux.txt` — Linux `perf record/report` hot-symbol output for the benchmark
  harness (`make perf-record`). It is useful as a hot-symbol profile only when the file says
  `No samples: no`, `Insufficient samples: no`, and the sample count meets the reported minimum.

## Policy

- No results are committed until produced by the benchmark harness (M11).
- Results include hardware, compiler, build type, and git commit.
- No estimated or fabricated numbers. They are synthetic microbenchmarks, hardware/compiler/
  build-dependent — not production throughput.

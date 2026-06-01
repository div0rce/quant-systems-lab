# Results

Benchmark results produced by `make bench` and scripts under `scripts/`.

- `latest.txt` — core microbenchmarks (`make bench`, `apps/qsl-bench`).
- `differential.txt` — differential-testing harness benchmarks: command-stream generation,
  gateway replay, and shrinking (`make bench-diff`, `qsl-bench diff`).
- `allocator_experiment.txt` — isolated M28 allocation experiment comparing `engine::Order`
  `new/delete` with fixed-pool acquire/release (`make bench-allocator`, `qsl-bench pool`).
- `perf_stat_linux.txt` — Linux `perf stat` output for the benchmark harness, when generated
  on a Linux host with PMU access (`make perf-stat`).
- `perf_report_linux.txt` — Linux `perf record/report` hot-symbol output for the benchmark
  harness (`make perf-record`).

## Policy

- No results are committed until produced by the benchmark harness (M11).
- Results include hardware, compiler, build type, and git commit.
- No estimated or fabricated numbers. They are synthetic microbenchmarks, hardware/compiler/
  build-dependent — not production throughput.

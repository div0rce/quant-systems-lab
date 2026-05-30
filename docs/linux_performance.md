# Linux Performance Notes

How to measure and reason about this project's performance honestly. The committed benchmark
numbers (`results/`) are a reproducible baseline, not a production-latency claim.

## Build mode and flags

- Benchmark only the **bench** preset, which inherits the Release configuration
  (`-O2`/`-O3`, `NDEBUG`) and disables tests. Debug numbers are meaningless for latency.
- Sanitizer builds (ASan/UBSan, see `make asan`) are for correctness, not timing — they add
  large, uneven overhead.

## Why numbers are hardware- and environment-dependent

- **CPU frequency scaling / turbo**: clocks vary with thermal and power state. For stable
  numbers, pin the governor to `performance` (`cpupower frequency-set -g performance`) and be
  aware turbo can still move results run to run.
- **Core sharing / scheduling**: a shared machine adds jitter. Pinning to an isolated core
  (`taskset -c <cpu>`, `isolcpus=`) reduces variance; the committed numbers do **not** do this.
- **Cache and allocator effects**: the order book uses `std::map`/`std::list` and heap
  allocation; cache locality and allocator behavior dominate small-op latency. Custom
  allocators / flat structures (backlog in `MILESTONES.md`) would change the picture.
- **Wall-clock vs logical time**: timing uses `std::chrono::steady_clock` at the benchmark
  layer only. The engine itself is logical-time and deterministic, so results are not affected
  by clock resolution beyond the measurement boundary.

## Reporting

- Report `p50`/`p95`/`p99`, not just a mean, when latency distribution matters. The current
  harness reports mean ns/op over many iterations as a first-order baseline; percentile
  reporting is a documented follow-up.
- Always record hardware, OS, compiler, build type, and git commit alongside the numbers —
  `scripts/run_benchmarks.sh` does this automatically.

## Measuring deeper

- `perf stat ./build/bench/qsl-bench` for cycles, instructions, IPC, cache misses, branch
  mispredictions.
- `perf record` / `perf report` (or a flamegraph) to find hot paths before optimizing.

## What this does not prove

These are in-process microbenchmarks on a commodity machine with the standard library and a
general-purpose allocator. They are useful for regression detection and honest, order-of-
magnitude framing — not evidence of production trading-system latency.

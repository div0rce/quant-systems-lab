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
- **Scheduler migration**: if a benchmark thread moves across cores, cache warmth and run-to-run
  variance can change independently of application logic. Future M43 work should record migration
  evidence where Linux exposes it and label hosts that cannot provide it.
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

- `make perf-stat` runs `scripts/perf_stat.sh` on Linux and records cycles, instructions,
  branch/cache events, context switches, and page faults when the host exposes those counters.
- `make perf-record` runs `scripts/perf_record.sh` on Linux and records a `perf report --stdio`
  software sampling report by default; it is a hot-symbol profile only when the recorded sample
  count clears the threshold reported in the artifact.
- See `docs/perf_analysis.md` for the M29 profiling workflow, artifacts, and caveats.

Current M29 artifacts are constrained-environment validation from Docker Desktop Linux. They prove
the workflow and metadata path, not real hardware PMU access. Full PMU-backed evidence is tracked
by issue #90 and requires bare-metal Linux or a Linux VM/server that exposes the required hardware
counters without permission or unsupported-counter errors.

## CPU affinity and locality studies

The future M43 study owns CPU affinity and NUMA/locality evidence. Acceptable measurements may use
`taskset`, `perf stat` context-switch/migration counters where available, and a tightly scoped
`pthread_setaffinity_np` probe if code-level pinning is justified. Artifacts must record topology,
kernel, compiler/build, command lines, git commit, and whether the host exposes the required
hardware data.

Unsupported or constrained hosts are valid outcomes. macOS, Docker Desktop, restricted CI, and
single-NUMA-node Linux machines should be labeled as constrained rather than used to imply NUMA or
production-latency evidence.

## What this does not prove

These are in-process microbenchmarks on a commodity machine with the standard library and a
general-purpose allocator. They are useful for regression detection and honest, order-of-
magnitude framing — not evidence of production trading-system latency.

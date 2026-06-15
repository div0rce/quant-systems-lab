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
  variance can change independently of application logic. M43 records migration evidence where
  Linux exposes it and labels hosts that cannot provide it.
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
- Always record hardware, OS, compiler, build type, and artifact provenance alongside the
  numbers. For migrated artifacts, `Source digest` is the stable identity and `Git commit
  (informational)` is not a stale-artifact signal by itself.

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

M43 owns CPU affinity and NUMA/locality evidence. Run:

```bash
make numa-study
```

This builds the benchmark preset and runs `scripts/numa_affinity_study.sh`. The script records an
unpinned benchmark run and a `taskset`-pinned run, then attempts `perf stat` software counters for
`context-switches` and `cpu-migrations`. It also records `lscpu` output and `numactl --hardware`
when available.

The artifact self-classifies its evidence:

- `full-linux-numa` — NUMA-capable Linux host with `taskset`, `numactl` topology, successful
  node-local and remote-memory binding attempts, and captured unpinned and pinned scheduler
  counters.
- `linux-constrained` — Linux host where at least one required topology or scheduler signal is
  unavailable. Commit only when intentionally documenting the constraint.
- `unsupported-host` — non-Linux host; no CPU-affinity, scheduler-migration, or NUMA evidence.

Use `QSL_NUMA_ALLOW_CONSTRAINED=1` only when the committed result is intentionally constrained.
Use `QSL_NUMA_CPU=<cpu>` to pin a specific CPU; otherwise the script picks the first CPU allowed by
the current cpuset.

Unsupported or constrained hosts are valid outcomes. macOS, Docker Desktop, restricted CI,
single-NUMA-node Linux machines, and hosts that can pin a CPU but cannot bind local/remote NUMA
memory should be labeled as constrained rather than used to imply full NUMA or production-latency
evidence.

## False-sharing studies

M44 owns the SPSC cursor false-sharing study. Run:

```bash
make false-sharing-study
```

This builds the benchmark preset and runs `scripts/run_false_sharing_study.sh`, which records a
benchmark-only packed-vs-padded SPSC queue-cursor comparison in
`results/false_sharing_study.txt`. The study uses the same producer-owned `tail` /
consumer-owned `head` release/acquire observation pattern as the production `SpscRing`, but it does
not change the production ring layout. Treat the artifact as host-local cache-line contention
evidence; scheduler placement, CPU topology, and OS behavior can move the result.

## DPDK environment checks

M48 owns late-stage DPDK research. Run:

```bash
make dpdk-check
```

This writes `results/dpdk_environment.txt`. It is a non-mutating support check: it does not reserve
hugepages, load kernel modules, bind NICs, or send packets. Treat it as research/environment
evidence only unless a later prototype artifact records DPDK version, EAL arguments, hugepage
state, device binding, packet workload, and source provenance.

## What this does not prove

These are in-process microbenchmarks on a commodity machine with the standard library and a
general-purpose allocator. They are useful for regression detection and honest, order-of-
magnitude framing — not evidence of production trading-system latency.

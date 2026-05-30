# Benchmarking

Reproducible latency and throughput measurements for the deterministic core. All numbers are
produced by the committed harness (`apps/qsl-bench`) and written to `results/`; none are
hand-written.

## Policy

No performance number appears in the README, PROGRESS, résumé bullets, or any doc unless it
was produced by `make bench` and recorded under `results/`. Benchmark results are
**hardware-, compiler-, and build-dependent**; the committed `results/latest.txt` records the
machine and toolchain it came from, and a different machine will produce different numbers.

## Harness

`apps/qsl-bench/main.cpp` is a small custom harness (no external benchmark dependency):

- `latency(name, iters, op)` — runs `op` `iters` times after a warmup and reports ns/op and
  ops/sec.
- `throughput(name, items, reps, run)` — runs `run` (which processes `items` items) `reps`
  times after a warmup and reports ns/item and items/sec.

A `volatile` sink consumes each operation's result so the optimizer cannot elide the work.
Timing uses `std::chrono::steady_clock` — wall-clock at the **benchmark layer only**; the
deterministic engine never reads a clock.

`make bench` uses a benchmark-specific CMake preset (`bench`) with
`QSL_BUILD_TESTS=OFF` and `QSL_BUILD_BENCHMARKS=ON`. This keeps benchmark configuration
separate from test-only dependencies: Catch2 `FetchContent` is not entered or populated for
benchmark-only builds.

## Scenarios

| Benchmark                  | Measures                                                        |
|----------------------------|-----------------------------------------------------------------|
| `order_book add/mod/cancel`| single-symbol order-book op latency over a bounded price band   |
| `protocol encode+decode`   | binary `NewOrder` round-trip latency                            |
| `gateway session IOC`      | end-to-end in-process gateway latency (decode → risk → engine → encode) for one IOC order |
| `matching engine flow`     | command-application throughput over a synthetic multi-symbol flow |
| `replay command log`       | state-rebuild throughput replaying a recorded command log       |

## Deterministic seeds

The matching and replay scenarios use `replay::generate_flow(seed = 42, symbols = 4,
orders = 5000)` — a fixed `mt19937_64` seed, so the workload is identical run to run and on
any machine. Changing the seed or sizes changes the workload; the committed results state the
parameters used.

## Report format

`scripts/run_benchmarks.sh` writes `results/latest.txt` with a metadata header followed by the
harness output:

```text
Hardware:    <arch>
OS:          <kernel>
Compiler:    <version>
Build type:  Release
Git commit:  <short sha>
Dataset:     synthetic order flow (seed 42, 4 symbols)
Date:        <UTC timestamp>

Scenario / Metric / Result:
<one line per benchmark: ns/op + ops/sec, or ns/item + items/sec>
```

## Running

```bash
make bench   # configures + builds the bench preset, runs qsl-bench, writes results/latest.txt
```

## What these numbers do and do not prove

- They **do** give a reproducible, order-of-magnitude picture of the core's latency/throughput
  on a stated machine, useful for spotting regressions and for honest résumé framing.
- They **do not** represent production trading latency. There is no kernel-bypass networking,
  no CPU pinning or isolation, no hugepages, and timing includes allocator and `std::map`
  overhead. CPU frequency scaling/turbo and a shared machine add noise. See
  `docs/linux_performance.md` for how to reason about and tighten such measurements.

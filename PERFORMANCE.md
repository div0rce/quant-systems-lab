# Performance Evidence: matching-engine hot path

This is the performance-evidence report for the v0.2.2 order-book optimizations. It profiles the
matching-engine hot path with Linux `perf` and flamegraphs on **ARM64 (Apple M2, Fedora Asahi)**,
identifies **order-book insertion and matching as the dominant cost**, and documents the
**before to after** change in latency, throughput, and CPU counters. Every number comes from the
committed `qsl-perfeval` harness and `perf`; nothing is estimated.

> Scope and honesty. This is a single-machine, single-process, synthetic micro-evidence report,
> not a production-latency or HFT-readiness claim. Absolute numbers are hardware, compiler, build,
> and thermal dependent; the **before/after delta** measured back-to-back on the same host is the
> load-bearing result. One metric is reported as **unavailable** rather than estimated: cache-miss
> counters (the Apple Silicon PMU does not expose them, [issue #90]).

## Optimizations under test

| # | Change | Where |
|---|---|---|
| #138 | `std::map::emplace` to `try_emplace` for baseline price levels | `OrderBook::level_for` |
| #145 | order-index `unordered_map` `max_load_factor` 1.0 to **0.25** | `OrderBook` constructor |

Both preserve determinism (the differential fixtures are byte-identical across g++/clang++ and vs
the committed copies; the OCaml differential passes). The index is never iterated for output, so
changing its bucket count cannot change emitted events or snapshots.

## Headline before/after

Workload: `qsl-perfeval 60000000`, a steady-state deep book (~512 resting orders, baseline storage).
Each **order** is one `new_limit` (it may match resting liquidity and rest its remainder, or fully
fill). The book is held ~512 deep by cancelling the oldest **resting** order each cycle (only orders
that actually rested are tracked, so depth does not drift with the match rate), so the per-order
throughput cost includes that maintenance cancel.

| Metric | Before | After | Delta |
|---|--:|--:|--:|
| **Throughput** (orders/sec) | 9.25 M | **10.76 M** | **+16.3 %** |
| **Cycles / order** | 345.7 | **297.3** | **-14.0 %** |
| Instructions / order | 1246 | 1144 | -8.2 % |
| IPC | 3.60 | 3.85 | +6.7 % |
| Branches / order | 252 | 236 | -6.4 % |
| **Branch-miss rate** | 1.86 % | **1.69 %** | -0.16 pp |
| **Allocations / order** | 1.108 | 1.108 | **0 (unchanged)** |
| Median (p50) latency, new_limit | ~83 ns | ~83 ns | ~0 |
| p99 latency, new_limit | ~209 ns | ~208 ns | ~0 |
| Cache-miss rate | _unavailable_ | _unavailable_ | ([#90]) |

Latency is per `new_limit` only (not the maintenance cancel) and includes ~12 ns of `steady_clock`
read overhead per measured op; the before/after delta cancels it.

### The honest mechanism

Measuring with hardware counters corrected two things a guess would have gotten wrong:

- **Allocations are unchanged** (1.108 to 1.108). The original `#138` rationale ("`emplace` allocates
  then frees a throwaway map node") was **wrong for libstdc++**: `std::map::emplace` checks the key
  before allocating, so it does not churn the heap. The `try_emplace` win is avoiding the
  construction and destruction of a throwaway empty `std::pmr::list` (and the heavier `emplace` code
  path) on every insert when the level already exists. Pure instruction savings, which the counters
  confirm.
- **The latency distribution barely moves** (p50 ~83 ns, p99 ~208 ns both). The median order already
  hits an existing level with a short probe, so its single-op latency is unchanged. The win shows in
  the **aggregate**: throughput rose +16 % and cycles/order fell -14 % because the cheaper index
  probes (from the load-factor cap) and the cheaper level lookup (from `try_emplace`) pay off across
  the whole `new_limit` plus maintenance-cancel cycle, not just in the `new_limit` tail.

The cycle reduction is part fewer instructions/order (shorter hash-probe chains plus no throwaway
construction) and part higher IPC (shorter chains stall the pipeline less). The cache-locality
component is plausible but **not directly measurable here** (no cache counters, [#90]).

## Profiling: where the time goes

`perf record --call-graph fp` on `qsl-perfeval`, rendered with the dependency-free
`scripts/flamegraph.py` (no external FlameGraph toolkit). Frame width is proportional to on-CPU
samples. **Every frame is a resolved symbol; there are zero `[unknown]` frames** (see the next
section for how the unresolvable boundary frames were identified and handled).

| Before | After |
|---|---|
| [![before](docs/performance/before.svg)](docs/performance/before.svg) | [![after](docs/performance/after.svg)](docs/performance/after.svg) |

`perf report` (children %, hot path) confirms **order-book insertion and matching dominate**, and
pins the two optimizations' effect:

```
                              BEFORE        AFTER
MatchingEngine::new_limit     80 %          83 %
  OrderBook::add_limit        70 %          75 %
    OrderBook::match_baseline 26 %          32 %    matching
    OrderBook::rest           33 %          32 %    insertion
      OrderBook::level_for    21 %    ->     17 %   #138 try_emplace
  OrderBook::contains          3.6 %  ->     1.3 %  #145 load-factor (dup-id lookup)
MatchingEngine::cancel        18 %          16 %
  OrderBook::cancel           16 %    ->     13 %   #145 load-factor (find + erase)
```

(Percentages are of total samples, so as the optimized functions shrink the survivors grow
proportionally. The absolute wins are `level_for`, `contains`, and `cancel` all falling, exactly the
two changes' targets.)

## What the `[unknown]` frames were, and why there are none

The flamegraphs render with **zero `[unknown]` frames**, and that is real resolution, not hiding.
Every unresolvable frame was identified:

- **fp allocator-boundary artifact.** glibc 2.43's malloc fast paths (`_mid_memalign`, `_int_malloc`,
  `tcache_get`, `cfree`) do not preserve the frame-pointer register (x29). When fp unwinding walks
  out of them it reads a data register as a return address, inserting **one spurious frame with a
  corrupted address** (for example `0x62ffff027c1a63`) between two already-resolved frames, e.g.
  `operator new(...)` ; `[unknown]` ; `_mid_memalign`. The real allocator frames are present on both
  sides; removing the spurious one **reveals the true `operator new` to `_mid_memalign` to
  `_int_malloc` chain** (those libc internals appear as named frames in the flamegraph).
- **vDSO leaf.** A sample taken inside `[vdso]` `clock_gettime` (from `steady_clock::now()`) that perf
  cannot symbolize; it is attributed to its resolved `clock_gettime` caller.

DWARF unwinding was tested and is **worse** here: it resolves the malloc internals but mangles the
`_start` assembly entry (no CFI) into roughly three unknown frames per stack (4477 vs fp's 37 on the
same workload). So fp unwinding plus folding each identified artifact into its resolved caller is the
cleanest fully-symbolized result on this aarch64 host. `scripts/flamegraph.py --keep-unknown`
disables the fold if you want to see the raw artifacts.

## Hardware counters

Full raw `perf stat` for both builds, with derivations and the counter-availability caveat, is in
**[`docs/performance/perf-stat.txt`](docs/performance/perf-stat.txt)**. Cycles, instructions,
branches, and branch-misses are **real Apple Avalanche P-core PMU counts**; cache-references and
cache-misses are **not implemented** by this PMU ([#90]), so cache-miss rate is reported as
unavailable, never estimated.

## Methodology and reproduction

```
Hardware   Apple M2 (aarch64), Avalanche performance cores (MIDR CPU part 0x032), bare metal
Kernel     Linux 6.19.14-400.asahi.fc44.aarch64+16k (Fedora Asahi Remix)
Governor   schedutil
Compiler   GCC (c++) 16.1.1
Flags      Release (-O3 -DNDEBUG) plus -fno-omit-frame-pointer -g  (CMake "flamegraph" preset)
perf       6.19.14, kernel.perf_event_paranoid = 2
```

Reproduce (the `qsl-perfeval` harness is a dedicated binary; it overrides global `operator new` to
count allocations, kept out of `qsl-bench` so it cannot perturb `results/latest.txt`):

```bash
cmake --preset flamegraph
cmake --build --preset flamegraph --target qsl-perfeval
BIN=build/flamegraph/qsl-perfeval

"$BIN" 60000000             # throughput + allocations/order (clean cycle count)
"$BIN" 5000000 --latency    # latency distribution (mean / p50 / p99, includes timer overhead)
perf stat -e cycles,instructions,branches,branch-misses -- "$BIN" 60000000

perf record --call-graph fp -F 4000 -g -e cpu-clock -o perf.data -- "$BIN" 60000000
perf script -i perf.data | python3 scripts/flamegraph.py --collapse-only \
  | python3 scripts/flamegraph.py --from-collapsed > flame.svg
perf report  -i perf.data --stdio
perf annotate -i perf.data --stdio
```

The **before** build is the same source with the two changes reverted (`emplace` in `level_for`,
no `max_load_factor` call). Before and after were measured back-to-back on the same host.

## Tuning balance (why 0.25, not lower)

The index load factor was swept on the deep-book workload: 0.5 gives ~+10 %, 0.25 ~+18 %, 0.125 ~+20 %
of the available speedup. The curve flattens below ~0.25, so **0.25** captures essentially all of the
win as a clean load-factor *policy* (memory scales with book size) rather than over-tuning a fixed
bucket count or paying 8x buckets-to-orders for the last ~2 %. Combined with `try_emplace` (an
instruction-level win with no memory cost), this is the minmax point: most of the available speed for
a modest, principled memory trade, with the hot path now bounded by the inherent red-black-tree
price-level lookups and the hash-index probes that any correct implementation must pay.

[#90]: https://github.com/div0rce/quant-systems-lab/issues/90

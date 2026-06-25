# Performance Evidence — matching-engine hot path

This is the performance-evidence report for the v0.2.2 order-book optimizations. It profiles the
matching-engine hot path with Linux `perf` and flamegraphs on **ARM64 (Apple M2, Fedora Asahi)**,
identifies **order-book insertion and matching as the dominant cost**, and documents the
**before → after** change in latency, throughput, and CPU counters. Every number comes from the
committed `qsl-perfeval` harness and `perf`; nothing is estimated.

> Scope and honesty. This is a single-machine, single-process, synthetic micro-evidence report —
> not a production-latency or HFT-readiness claim. Absolute numbers are hardware/compiler/build/
> thermal dependent; the **before/after delta** measured back-to-back on the same host is the load-
> bearing result. Two metrics are reported honestly as **unavailable** rather than estimated:
> cache-miss counters (the Apple Silicon PMU does not expose them — [issue #90]) and any
> sub-`steady_clock`-resolution timing.

## Optimizations under test

| # | Change | Where |
|---|---|---|
| #138 | `std::map::emplace` → `try_emplace` for baseline price levels | `OrderBook::level_for` |
| #145 | order-index `unordered_map` `max_load_factor` 1.0 → **0.25** | `OrderBook` constructor |

Both preserve determinism (the differential fixtures are byte-identical across g++/clang++ and vs
the committed copies; the OCaml differential passes). The index is never iterated for output, so
changing its bucket count cannot change emitted events or snapshots.

## Headline before/after

Workload: `qsl-perfeval 60000000` — a steady-state deep book (~512 resting orders, baseline storage).
Each **order** is one `new_limit` (it may match resting liquidity and rest its remainder); the book
is held ~512 deep by cancelling the oldest order each cycle, so the per-order throughput cost
includes that maintenance cancel.

| Metric | Before | After | Δ |
|---|---|---|---|
| **Throughput** (orders/sec) | 8.89 M | **11.13 M** | **+25.2 %** |
| Median (p50) latency¹ | 83 ns | 83 ns | ~0 |
| **p99 latency**¹ | 250 ns | **208 ns** | **−16.8 %** |
| Mean latency¹ | 92 ns | 75 ns | −18.5 % |
| **Cycles / order** | 348.2 | **288.4** | **−17.2 %** |
| Instructions / order | 1239 | 1143 | −7.8 % |
| IPC | 3.56 | 3.96 | +11.4 % |
| Branches / order | 244 | 229 | −6.1 % |
| **Branch-miss rate** | 2.02 % | **1.81 %** | −0.21 pp |
| Cache-miss rate | _unavailable_ | _unavailable_ | — ([#90]) |
| **Allocations / order** | 1.106 | 1.106 | **0 (unchanged)** |

¹ Latency is per `new_limit` only (not the maintenance cancel) and includes ~12 ns of `steady_clock`
read overhead per measured op (two VDSO clock reads); the before/after delta cancels it. Throughput
is per full cycle (`new_limit` + cancel), which is why p50 (~83 ns) is below the per-cycle wall time
(1 / 8.89 M ≈ 112 ns before; ≈ 90 ns after).

### The honest mechanism

The win is **fewer cycles and instructions per order**, not fewer allocations:

- **Allocations are unchanged** (1.106 → 1.106). The original `#138` rationale ("`emplace` allocates
  then frees a throwaway map node") turned out to be **wrong for libstdc++** — `std::map::emplace`
  checks the key *before* allocating a node, so it does not churn the heap. The `try_emplace` win is
  avoiding the construction/destruction of a throwaway empty `std::pmr::list` (and the heavier
  `emplace` code path) on every insert when the level already exists — pure instruction savings,
  which the counters confirm. This correction is the whole point of measuring with hardware counters
  instead of guessing.
- **Shorter index probe chains.** Capping the order-index load factor at 0.25 keeps the
  `OrderId → Locator` hash table sparse, so each of the 1–4 lookups per order (`contains`, `cancel`
  find/erase, `rest` insert) probes fewer buckets. That shows up as lower instructions/order **and**
  higher IPC (+11.4 %) — shorter chains stall the pipeline/memory system less — and a lower
  branch-miss rate (fewer mispredicted bucket-traversal loop branches). The cache-locality component
  is plausible but **not directly measurable here** (no cache counters; [#90]).

## Profiling: where the time goes

`perf record --call-graph fp` on `qsl-perfeval`, rendered with the dependency-free
`scripts/flamegraph.py` (no external FlameGraph toolkit). Frame width ∝ on-CPU samples.

| Before | After |
|---|---|
| [![before](docs/performance/before.svg)](docs/performance/before.svg) | [![after](docs/performance/after.svg)](docs/performance/after.svg) |

`perf report` (children %, hot path) confirms **order-book insertion + matching dominate**, and pins
the two optimizations' effect:

```
                              BEFORE        AFTER
MatchingEngine::new_limit     80.1 %        83.2 %
  OrderBook::add_limit        69.5 %        74.7 %
    OrderBook::match_baseline 25.7 %        32.0 %   <- matching
    OrderBook::rest           33.3 %        31.8 %   <- insertion
      OrderBook::level_for    21.3 %  ->    17.5 %   <- #138 try_emplace
  OrderBook::contains          3.6 %  ->     1.3 %   <- #145 load-factor (dup-id lookup)
MatchingEngine::cancel        18.2 %        15.8 %
  OrderBook::cancel           16.0 %  ->    13.2 %   <- #145 load-factor (find + erase)
```

(Percentages are of total samples, so as the optimized functions shrink the survivors grow
proportionally — e.g. `new_limit` rises 80→83 % only because the total dropped. The *absolute* wins
are `level_for`, `contains`, and `cancel` all falling, exactly the two changes' targets.)

`perf annotate` attributes the remaining cost of `level_for` to the `std::_Rb_tree` lookup/insert
loads (`ldr`/`ldp` over the red-black-tree nodes) — the inherent cost of an ordered price-level map,
not avoidable allocation.

## Hardware counters

Full raw `perf stat` for both builds, with derivations and the counter-availability caveat, is in
**[`docs/performance/perf-stat.txt`](docs/performance/perf-stat.txt)**. Cycles, instructions,
branches, and branch-misses are **real Apple Avalanche P-core PMU counts**; cache-references /
cache-misses are **not implemented** by this PMU ([#90]), so cache-miss rate is reported as
unavailable, never estimated.

## Methodology / reproduction

```
Hardware   Apple M2 (aarch64), Avalanche performance cores (MIDR CPU part 0x032), bare metal
Kernel     Linux 6.19.14-400.asahi.fc44.aarch64+16k (Fedora Asahi Remix)
Governor   schedutil
Compiler   GCC (c++) 16.1.1
Flags      Release (-O3 -DNDEBUG) + -fno-omit-frame-pointer -g   (CMake "flamegraph" preset)
perf       6.19.14, kernel.perf_event_paranoid = 2
```

Reproduce (the `qsl-perfeval` harness is a dedicated binary — it overrides global `operator new` to
count allocations, kept out of `qsl-bench` so it cannot perturb `results/latest.txt`):

```bash
cmake --preset flamegraph
cmake --build --preset flamegraph --target qsl-perfeval
BIN=build/flamegraph/qsl-perfeval

# throughput + allocations/order (clean: no per-op timer in the cycle count)
"$BIN" 60000000

# latency distribution (mean / p50 / p99; includes timer overhead, reported)
"$BIN" 5000000 --latency

# hardware counters
perf stat -e cycles,instructions,branches,branch-misses -- "$BIN" 60000000

# flamegraph
perf record --call-graph fp -F 4000 -g -e cpu-clock -o perf.data -- "$BIN" 60000000
perf script -i perf.data | python3 scripts/flamegraph.py --collapse-only \
  | python3 scripts/flamegraph.py --from-collapsed > flame.svg

# hot path / annotation
perf report  -i perf.data --stdio
perf annotate -i perf.data --stdio
```

The **before** build is the same source with the two changes reverted (`emplace` in `level_for`,
no `max_load_factor` call). Before and after were measured back-to-back on the same host.

## Tuning balance (why 0.25, not lower)

The index load factor was swept on the profile workload: 0.5 → ~+10 %, 0.25 → ~+18 %, 0.125 → ~+20 %.
The curve flattens below ~0.25, so **0.25** captures essentially all of the throughput win as a clean
load-factor *policy* (memory scales with book size) rather than over-tuning a fixed bucket count or
paying 8× buckets-to-orders for the last ~2 %. Combined with `try_emplace` (an instruction-level win
with no memory cost), this is the minmax point: most of the available speed for a modest, principled
memory trade, with the hot path now bounded by the inherent red-black-tree price-level lookups and
the hash-index probes that any correct implementation must pay.

[#90]: https://github.com/div0rce/quant-systems-lab/issues/90

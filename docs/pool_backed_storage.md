# Order-Book Storage Experiments

M32 and M47 evaluate order-book storage in the actual matching-engine path. These are
engine-level experiments, not isolated allocator microbenchmarks.

## Baseline Storage

The current order book stores:

- price levels in ordered maps (`std::map<Price, Level>`);
- per-price FIFO order queues in `std::list<Order>`;
- active-order locators in `std::unordered_map<OrderId, Locator>`.

This design is simple and deterministic. Price maps preserve best-price traversal order, the list
preserves FIFO priority inside each level, and the unordered locator is never iterated for matching
or snapshot generation.

## Why M32 Did Not Directly Use OrderPool

M28 introduced `OrderPool<Capacity>`, a raw-storage pool for constructing and destroying
`engine::Order` objects with explicit lifetime management. That pool manages bare `engine::Order`
slots.

The current order book does not allocate bare `engine::Order` objects directly. `std::list<Order>`
allocates implementation-defined list nodes that contain an `Order` plus links and allocator
bookkeeping. A raw `OrderPool<Capacity>` cannot honestly back those list-node allocations.

Direct `OrderPool<Capacity>` integration would require a custom or intrusive order-node design:
pool-owned nodes, explicit links, locator rewrites, and careful preservation of price-time
priority. M32 deliberately avoided that larger storage-architecture change and used PMR instead.

## M32 PMR Design

M32 keeps the container design and changes only the allocator source:

- `OrderBook::Storage::Baseline` uses `std::pmr::new_delete_resource`;
- `OrderBook::Storage::Pooled` owns a per-book `std::pmr::unsynchronized_pool_resource`;
- per-price FIFO lists are `std::pmr::list<Order>`;
- bid/ask levels are `std::pmr::map`;
- active-order locators are `std::pmr::unordered_map`;
- `MatchingEngine(OrderBook::Storage)` propagates the chosen mode to every per-symbol book.

The storage mode is fixed at construction. It does not change matching rules, sequencing, replay,
or snapshot ordering.

## Intrusive OrderPool Mode

The follow-up intrusive experiment adds `OrderBook::Storage::IntrusivePooled`. It keeps the same
public `OrderBook` and `MatchingEngine(OrderBook::Storage)` surface, but replaces the per-price
`std::list<Order>` FIFO queues with custom linked nodes:

- each resting `engine::Order` is constructed in the M28 `OrderPool<Capacity>`;
- each FIFO link node is allocated from a fixed raw node pool;
- price levels still live in ordered maps, so best-price traversal is unchanged;
- the active-order locator maps `OrderId` to the custom node, side, and price;
- the storage mode is explicit and opt-in; `Baseline` remains the default.

This closes issue #95's direct-storage follow-up at the order-node level without claiming a full
flat/contiguous order book. Level maps and locator maps are still standard containers.

## M47 Contiguous Direct-Price-Indexed Mode

M47 adds `OrderBook::Storage::Contiguous`, an opt-in layout study for a direct price-indexed book.
It replaces the ordered price maps with a fixed price band:

```text
[OrderBook::kContiguousMinPrice, OrderBook::kContiguousMaxPrice] == [1, 1024]
```

Within that band:

- each side owns a flat array of price levels;
- occupancy bitmaps find the next occupied level best-first;
- each price level stores FIFO resting orders in a contiguous `std::vector`;
- canceled and filled orders become tombstones and long-lived levels compact when tombstones
  dominate;
- active-order locators point to `{side, price, position}` inside the flat level.

The price band is an explicit study assumption, not a production-domain claim. Out-of-band limit
prices may still cross existing in-band liquidity; the band constrains where GTC remainders may
rest. A GTC order whose remainder would rest outside the band is refused before engine mutation via
`can_store_limit`, similar to how intrusive pooled storage can refuse a residual when fixed
capacity is exhausted. Modifies are pre-gated the same way: the engine asks `can_apply_modify`
before emitting `OrderModified`, so a reprice whose cancel-and-re-add remainder would rest out of
band is refused with no event and no state change (the gateway reports it as `StorageExhausted`,
and the original order keeps resting at its old price). The event stream therefore never reports a
modify the book did not apply.

This is not a general sparse-price-book replacement. It is a cache/locality experiment for bounded
integer-tick domains where direct indexing is plausible.

## Affected Allocations

`Storage::Pooled` affects container node allocation inside each `OrderBook`:

- map nodes for price levels;
- list nodes for resting orders;
- unordered-map nodes/buckets for order locators;
- allocator bookkeeping inside the PMR pool resource.

`Storage::IntrusivePooled` affects:

- resting `engine::Order` lifetime and storage through `OrderPool<Capacity>`;
- per-price FIFO link nodes through a fixed raw node pool;
- locator entries, which now point to custom nodes instead of `std::list<Order>` iterators.

It does not affect:

- protocol frame allocation;
- gateway/session buffers;
- event-log allocation;
- market-data publisher allocation;
- `std::vector<EngineEvent>` result allocation;
- `std::map<SymbolId, OrderBook>` nodes owned by `MatchingEngine`.
- price-level map allocation or locator hash-table allocation in intrusive mode.

`Storage::Contiguous` affects:

- bid/ask price-level storage, replacing ordered maps with fixed direct-index arrays;
- level discovery, replacing tree traversal with occupancy bitmap scans;
- per-price FIFO storage, replacing linked list nodes with contiguous vectors;
- locator representation, replacing list iterators/custom node pointers with vector positions.

It still does not affect:

- protocol frame allocation;
- gateway/session buffers;
- event-log allocation;
- market-data publisher allocation;
- `std::vector<EngineEvent>` result allocation;
- `MatchingEngine`'s per-symbol map;
- hash-table allocation for active-order locators.

## Command Hot Path

The M47 follow-up keeps the comparison explicit about what each storage mode changes and what it
leaves in the command path:

| Mode | Price-level structure | Per-level FIFO | Locator | Allocation path | Best bid/ask path | Cancel/modify path |
| --- | --- | --- | --- | --- | --- | --- |
| Baseline | `std::pmr::map` using `new_delete_resource` | `std::pmr::list<Order>` | `std::pmr::unordered_map<OrderId, Locator>` | General heap through the default PMR resource | `map.begin()` | Hash lookup, list erase, map-level erase if empty; priority-losing modify cancels and re-adds |
| PMR pooled | Same maps as baseline | Same lists as baseline | Same PMR hash map as baseline | Per-book `std::pmr::unsynchronized_pool_resource` for map/list/hash nodes | `map.begin()` | Same algorithm as baseline, but node allocations come from the PMR pool |
| Intrusive pooled | `std::map<Price, Level>` | Custom linked nodes plus `OrderPool<Capacity>` | `std::unordered_map<OrderId, Locator>` to custom nodes | Fixed raw pools for orders and FIFO nodes; map/hash allocations remain standard | `map.begin()` | Hash lookup, custom unlink/release; storage preflight only simulates matches when the pool is full |
| Contiguous | Fixed direct-index arrays for `[1, 1024]` plus occupancy bitmaps | Per-level `std::vector<FlatOrder>` with tombstones/compaction | `std::unordered_map<OrderId, Locator>` to vector positions | Vectors and locator hash table still allocate; price-level map/list nodes are removed | Occupancy bitmap scan to best occupied slot | Hash lookup, tombstone/compact, vector-position locator updates; out-of-band residuals are refused before mutation |

Contiguous storage removes the ordered price maps and `std::list<Order>` nodes, but it does not
remove event-vector allocation, `MatchingEngine`'s symbol map, gateway/risk paths, or the active
order hash table. The benchmark is therefore an engine-workload comparison, not a pure
price-array microbenchmark.

## Correctness Gate

The tests replay deterministic generated flows through all storage modes and assert:

- identical per-command event streams;
- identical aggregate event stream;
- identical `last_seq`;
- identical final `EngineSnapshot`;
- non-vacuity: trades occur and resting liquidity remains.

The M47 follow-up adds a small all-mode benchmark-mix regression rather than duplicating the whole
engine test suite. It covers the command mix that matters for this benchmark family: partial fills,
duplicate active IDs, IOC orders, market orders, cancels, modifies, event-stream equality, final
snapshot equality, and nonempty resting state.

The generated-flow test prices remain inside the contiguous mode's explicit band, so it compares
storage layout without intentionally triggering out-of-band storage refusals. This keeps the
experiments as storage-layout studies, not semantic changes inside the declared domain.

## Benchmark Methodology

Run:

```bash
make bench-storage
```

The script builds the release benchmark preset and writes `results/pool_backed_storage.txt`. It now
runs five deterministic workloads through `MatchingEngine` in baseline, PMR pooled, intrusive
pooled, and contiguous direct-indexed modes:

- general generated flow: the existing generated workload (`seed = 42`);
- dense bounded flow: many live levels in a small bounded domain plus top-of-book probes;
- sparse wide flow: few active levels spread across a wide in-band price domain;
- cancel/modify-heavy flow: locator-heavy churn with duplicate active IDs;
- match/traversal-heavy flow: many small maker orders swept across levels.

Each workload prints a non-timed shape line before timing. Timing reports median, minimum, and
maximum `ns/command` across 30 full workload replays per storage mode. Shape collection is outside
the measured loop. Top-of-book probes are included only when listed and are intentional workload
operations, not instrumentation.

This benchmark exercises the engine path. It is not isolated allocator acquire/release and does not
include network, disk, Linux kernel path, or perf/PMU counters.

## Result Interpretation

Treat committed results as local measurements, not general speedup claims. Allocator behavior
depends on CPU, compiler, standard library, allocation pattern, cache state, and build mode.

The original M47 artifact for the single generated flow ranked PMR fastest, contiguous second,
baseline third, and intrusive slowest:

```text
PMR 209.6 ns/cmd < contiguous 222.4 < baseline 273.7 < intrusive 373.0
```

The M47 follow-up artifact was regenerated in the local Linux-container toolchain after the
diagnosis changes (`Source digest:
sha256:3c9de2760a695bacb1dd47637e51182ac19cfa14876abb1bac57d76a4d4369c6`,
`Dirty inputs: no`). It preserves the original PMR-fastest result as historical evidence and adds
workload-sensitive evidence:

| Workload | Shape summary | Median ranking, fastest to slowest |
| --- | --- | --- |
| General generated flow | 5004 commands, 2238 trades, 793 cancels, 690 modifies, 602 market orders, 376 IOC orders, max 41 active levels, width 67, density 0.076 | Contiguous 105.8 ns/cmd, baseline 130.0, PMR 210.9, intrusive 591.5 |
| Dense bounded flow | 5004 commands, 1048 trades, 984 market orders, 492 modifies, 492 IOC orders, 20016 top-of-book probes, max 80 active levels, width 136, density 0.147 | Contiguous 83.2, PMR 112.3, baseline 120.7, intrusive 336.2 |
| Sparse wide flow | 5004 commands, no trades, 828 cancels, 828 modifies, max 32 active levels, width 985, density 0.004 | Contiguous 72.4, baseline 105.3, PMR 155.8, intrusive 725.0 |
| Cancel/modify-heavy flow | 5004 commands, no trades, 1599 cancels, 1603 modifies, max 60 active levels, width 30, density 0.333 | Contiguous 65.0, PMR 65.5, baseline 81.2, intrusive 337.1 |
| Match/traversal-heavy flow | 5004 commands, 4012 trades, 494 market orders, 494 IOC orders, max 60 active levels, width 81, density 0.370 | Contiguous 76.7, PMR 127.9, baseline 134.0, intrusive 145.8 |

This evidence supports a workload, allocator, and environment-sensitivity interpretation rather than
a semantic problem. The original single-flow artifact was not a pure cache-locality benchmark: it
included symbol routing, duplicate checks, locator updates, event emission, cancel/modify
bookkeeping, and allocation behavior. PMR can win that kind of workload when pooled node allocation
dominates and the standard-container path stays simpler than custom storage. The Linux-container
follow-up did not reproduce that PMR win: contiguous storage led the median for every named
workload, with the cancel/modify-heavy result effectively a near-tie against PMR. That makes the
safe conclusion narrower: contiguous can benefit bounded in-band workloads, especially traversal
heavy ones, but the original PMR-fastest result shows that ranking is not portable across
environment and workload shape.

The sparse-wide workload also favors contiguous storage in this fixed in-band study, but that does
not make direct indexing a general sparse-price-book replacement. The benchmark deliberately keeps
resting prices inside `[1, 1024]`; a wider or unbounded production-style price domain would change
the memory and fallback tradeoffs.

Intrusive pooled storage remains slower in every follow-up workload. The follow-up removed one real
overhead: when capacity exists, `can_store_limit` now returns before simulating a match. The
remaining regression is an overhead tradeoff: intrusive storage still uses ordered price maps and a
hash locator, adds custom linked-node management, and does not get PMR's broad pooling for map and
hash nodes.

## Cache, Locality, And Replay

PMR pooling can improve locality by drawing repeated container node allocations from pooled chunks,
but it does not make the order book contiguous. Intrusive pooled storage removes `std::list` nodes
from the hot FIFO path, but it still traverses linked nodes and ordered price maps; it is not a
flat-array book. Contiguous storage changes the price-level layout and per-level FIFO locality, but
its fixed price band is a tradeoff: direct indexing can be attractive in bounded integer domains
and wasteful or semantically awkward in sparse/unbounded domains.

Replay determinism is unchanged because allocation addresses are not part of the domain state,
snapshot output, event ordering, or sequence assignment.

## Limitations

- PMR does not eliminate all allocations in the engine path.
- Intrusive pooled storage does not eliminate price-level or locator-map allocations.
- The intrusive pool has fixed order-node capacity and no heap fallback; committed workloads stay
  within that capacity.
- Contiguous storage has a fixed resting-price band and is not suitable for arbitrary sparse price
  domains without a fallback or a different indexing strategy.
- The benchmark is synthetic and single-process.
- No hardware PMU/cache-miss evidence was collected for this artifact; it was generated in Docker
  Desktop Linux, which is constrained evidence rather than a bare-metal perf run.
- The result is not a production-latency claim.
- The experiment does not prove that any storage mode should be retained for every workload.

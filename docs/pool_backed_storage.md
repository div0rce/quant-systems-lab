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
capacity is exhausted.

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

## Correctness Gate

The invariant test replays deterministic generated flows through all storage modes and asserts:

- identical per-command event streams;
- identical aggregate event stream;
- identical `last_seq`;
- identical final `EngineSnapshot`;
- non-vacuity: trades occur and resting liquidity remains.

The generated-flow test prices remain inside the contiguous mode's explicit band, so it compares
storage layout without intentionally triggering out-of-band storage refusals. This keeps the
experiments as storage-layout studies, not semantic changes inside the declared domain.

## Benchmark Methodology

Run:

```bash
make bench-storage
```

The script builds the release benchmark preset and writes `results/pool_backed_storage.txt`. The
workload is a deterministic generated flow (`seed = 42`, `symbols = 4`, `orders = 5000`) replayed
through `MatchingEngine` in baseline, PMR pooled, intrusive pooled, and contiguous direct-indexed
modes. The metric is `ns/command` and `commands/sec`.

This benchmark exercises the engine path. It is not isolated allocator acquire/release and does not
include network, disk, Linux kernel path, or perf/PMU counters.

## Result Interpretation

Treat committed results as local measurements, not general speedup claims. Allocator behavior
depends on CPU, compiler, standard library, allocation pattern, cache state, and build mode.

The committed M47 artifact is a bounded-domain storage comparison on one macOS/Apple clang host.
It shows the contiguous row in the same broad range as baseline/PMR for this generated flow, while
the intrusive row is slower on this host. That is useful evidence, not a portable conclusion. The
question is whether a storage mode improves this specific engine workload without changing
semantics inside the mode's declared domain.

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
- The result is not a production-latency claim.
- The experiment does not prove that any storage mode should be retained for every workload.

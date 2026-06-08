# Pool-Backed Order-Book Storage

M32 evaluates order-book storage allocation in the actual matching-engine path. It is an
engine-level experiment, not another isolated allocator microbenchmark.

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

## Correctness Gate

The invariant test replays deterministic generated flows through all storage modes and asserts:

- identical per-command event streams;
- identical aggregate event stream;
- identical `last_seq`;
- identical final `EngineSnapshot`;
- non-vacuity: trades occur and resting liquidity remains.

This keeps M32 as an allocation experiment, not a semantic change.

## Benchmark Methodology

Run:

```bash
make bench-storage
```

The script builds the release benchmark preset and writes `results/pool_backed_storage.txt`. The
workload is a deterministic generated flow (`seed = 42`, `symbols = 4`, `orders = 5000`) replayed
through `MatchingEngine` in baseline, PMR pooled, and intrusive pooled modes. The metric is
`ns/command` and `commands/sec`.

This benchmark exercises the engine path. It is not isolated allocator acquire/release and does not
include network, disk, Linux kernel path, or perf/PMU counters.

## Result Interpretation

Treat committed results as local measurements, not general speedup claims. Allocator behavior
depends on CPU, compiler, standard library, allocation pattern, cache state, and build mode.

If future runs show neutral or slower pooled performance, that is still a valid result. The
question is whether a storage mode improves this specific engine workload without changing
semantics.

## Cache, Locality, And Replay

PMR pooling can improve locality by drawing repeated container node allocations from pooled
chunks, but it does not make the order book contiguous. Intrusive pooled storage removes
`std::list` nodes from the hot FIFO path, but it still traverses linked nodes and ordered price
maps; it is not a flat-array book.

Replay determinism is unchanged because allocation addresses are not part of the domain state,
snapshot output, event ordering, or sequence assignment.

## Future Contiguous Storage Study

M47 owns the next storage-architecture question: whether flat/contiguous order-book layouts provide
better cache-locality tradeoffs than the baseline, PMR pooled, or intrusive pooled modes. Candidate
approaches include flat-vector-style price levels, direct price-index storage where price domains
are explicit, and other contiguous layouts that preserve price-time priority.

That work must be an engine-level study, not another isolated allocator benchmark. It needs replay
or differential equivalence, benchmark artifacts, and cache-locality discussion. A slower or neutral
result is acceptable if it explains the tradeoff honestly.

## Limitations

- PMR does not eliminate all allocations in the engine path.
- Intrusive pooled storage does not eliminate price-level or locator-map allocations.
- The intrusive pool has fixed order-node capacity and no heap fallback; committed workloads stay
  within that capacity.
- The benchmark is synthetic and single-process.
- The result is not a production-latency claim.
- The experiment does not prove that pooled allocation should be retained for every workload.

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

## Why Not Direct OrderPool Integration

M28 introduced `OrderPool<Capacity>`, a raw-storage pool for constructing and destroying
`engine::Order` objects with explicit lifetime management. That pool manages bare `engine::Order`
slots.

The current order book does not allocate bare `engine::Order` objects directly. `std::list<Order>`
allocates implementation-defined list nodes that contain an `Order` plus links and allocator
bookkeeping. A raw `OrderPool<Capacity>` cannot honestly back those list-node allocations.

Direct `OrderPool<Capacity>` integration would require a custom or intrusive order-node design:
pool-owned nodes, explicit links, locator rewrites, and careful preservation of price-time
priority. That follow-up is tracked in issue #95.

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

## Affected Allocations

`Storage::Pooled` affects container node allocation inside each `OrderBook`:

- map nodes for price levels;
- list nodes for resting orders;
- unordered-map nodes/buckets for order locators;
- allocator bookkeeping inside the PMR pool resource.

It does not affect:

- protocol frame allocation;
- gateway/session buffers;
- event-log allocation;
- market-data publisher allocation;
- `std::vector<EngineEvent>` result allocation;
- `std::map<SymbolId, OrderBook>` nodes owned by `MatchingEngine`.

## Correctness Gate

The invariant test replays deterministic generated flows through both storage modes and asserts:

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
through `MatchingEngine` in baseline and pooled modes. The metric is `ns/command` and
`commands/sec`.

This benchmark exercises the engine path. It is not isolated allocator acquire/release and does not
include network, disk, Linux kernel path, or perf/PMU counters.

## Result Interpretation

On the committed macOS/Apple-clang run, pooled PMR storage measured faster than baseline for this
synthetic engine flow. Treat that as a local measurement, not a general speedup claim. Allocator
behavior depends on CPU, compiler, standard library, allocation pattern, cache state, and build
mode.

If future runs show neutral or slower PMR performance, that is still a valid M32 result. The
question is whether pooled node allocation improves this specific engine workload without changing
semantics.

## Cache, Locality, And Replay

PMR pooling can improve locality by drawing repeated container node allocations from pooled
chunks, but it does not make the order book contiguous. The book is still map/list based, with
pointer-heavy traversal and cache misses typical of node containers.

Replay determinism is unchanged because allocation addresses are not part of the domain state,
snapshot output, event ordering, or sequence assignment.

## Limitations

- PMR is not an intrusive order-node design.
- PMR does not eliminate all allocations in the engine path.
- The benchmark is synthetic and single-process.
- The result is not a production-latency claim.
- The experiment does not prove that pooled allocation should be retained for every workload.

## Future Work

Issue #95 tracks direct intrusive/custom-node order-book storage. That work would replace the
`std::list<Order>` node model with explicit pool-owned order nodes, preserve price-time priority
and locator correctness, and benchmark against both baseline and M32 PMR storage.

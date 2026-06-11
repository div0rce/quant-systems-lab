# Memory Pool Allocator Experiment

M28 is a scoped allocation experiment for hot-path order-like objects. It does not replace the
order book's `std::list` storage, and it does not claim an end-to-end engine speedup. The point is
to measure whether a fixed-capacity pool can reduce object allocation overhead compared with
ordinary heap allocation.

## Pool Contract

`qsl::memory::OrderPool<Capacity>` stores raw, aligned bytes for `engine::Order` slots:

- capacity is fixed at compile time;
- slots are not default-constructed when the pool is created;
- `try_acquire(...)` constructs a fresh `engine::Order` in the selected slot;
- `try_acquire(...)` returns `nullptr` when the pool is exhausted;
- `release(ptr)` destroys the live object before returning the slot to the free list;
- `reset()` destroys every live object before rebuilding the free list;
- the pool destructor destroys any still-live objects;
- there is no heap fallback and no growth path;
- `release(ptr)` returns `false` for null, non-owned, interior/misaligned, or
  already-released pointers.

This makes failure explicit. A caller must size the pool and handle exhaustion; silently falling
back to the heap would invalidate the experiment. The pool is deliberately still isolated from the
order book; it is allocator evidence, not an order-book storage refactor.

## Benchmark

Run:

```bash
make bench-allocator
```

The target builds the benchmark preset and runs:

```bash
qsl-bench pool
```

`scripts/run_allocator_experiment.sh` writes `results/allocator_experiment.txt` with hardware,
OS, compiler, build type, source-digest provenance, date, and the measured scenarios:

- `order new/delete` — baseline heap allocation for one `engine::Order`;
- `order pool acquire/release` — one acquire and release against a fixed pool;
- `order pool burst cycle` — fill a 1024-slot pool, then release all slots.

## Interpreting Results

The result is an isolated allocation microbenchmark. It excludes matching, risk, protocol,
networking, logging, and kernel paths. It is useful as evidence for whether a pool is worth
considering in a future order-book storage design, but it is not an engine-latency claim.

A negative result is acceptable. If the pool path is not meaningfully faster on a given machine,
the correct conclusion is to keep the simpler order-book storage until broader profiling proves
allocation is a real bottleneck.

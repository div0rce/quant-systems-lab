# Concurrency model

> Phase III, M24–M25. This documents the first concurrency primitive, the bounded SPSC ring buffer
> (`include/qsl/concurrency/spsc_ring.hpp`): why it is single-producer/single-consumer, who owns
> what, how producer and consumer observe each other, the backpressure and shutdown contract the
> later pipeline depends on, and the honest limits. The C++ memory-model details (per-step
> acquire/release reasoning and the happens-before proof) live in
> [`memory_ordering.md`](memory_ordering.md). The threaded pipeline that consumes this queue is
> M26.
>
> The behavioral claims here are exercised by
> [`tests/concurrency/test_spsc_stress.cpp`](../tests/concurrency/test_spsc_stress.cpp) and
> [`tests/concurrency/test_backpressure.cpp`](../tests/concurrency/test_backpressure.cpp).

## Why SPSC, not MPMC

The planned threaded pipeline (M26) is a chain of single-threaded stages connected by
hand-off queues:

```text
input thread  --[inbound queue]-->  engine thread  --[outbound queue]-->  publisher/log thread
```

Each queue has **exactly one producer and one consumer**. That is the defining condition for a
single-producer/single-consumer (SPSC) queue, so an MPMC queue would add compare-and-swap
contention, retry loops, and ABA/hazard concerns for a problem we do not have. SPSC lets each
index be owned by exactly one thread, which keeps the synchronization to a single acquire/release
pair per direction and the operations wait-free (see `memory_ordering.md`).

`SpscRing<T, Capacity>` is therefore intentionally **not** a general concurrent container:
concurrent producers, or concurrent consumers, are undefined behavior.

## Ownership

Two cursors index a fixed array of `kSlots = Capacity + 1` slots. Each cursor has a single writer:

| State                  | Written by | Read by                                |
| ---------------------- | ---------- | -------------------------------------- |
| `tail_` (write cursor) | producer   | producer (relaxed), consumer (acquire) |
| `head_` (read cursor)  | consumer   | consumer (relaxed), producer (acquire) |
| `buffer_[i]`           | producer   | consumer (after `tail_` is published)  |

### Slot lifecycle

A single slot moves through a strict ownership cycle; it is never written by one thread while read
by the other:

```text
        producer owns                         consumer owns
   ┌───────────────────────┐            ┌───────────────────────┐
   │ write buffer_[tail]    │  publish   │ read buffer_[head]     │  publish
   │ (plain store)          │ ─tail_──▶  │ (plain load)           │ ─head_──▶  (free, reusable)
   └───────────────────────┘  release   └───────────────────────┘  release
```

- The producer owns a slot until it publishes `tail_` with `release`. Before that point the
  consumer cannot observe the slot as non-empty, so it never reads it.
- The consumer owns the slot until it publishes `head_` with `release`. Before that point the
  producer's full check has not seen the slot freed, so the one spare slot guarantees the producer
  cannot reuse (overwrite) it.

This two-phase hand-off is the entire correctness argument; the memory orderings that make each
publication visible are tabulated in `memory_ordering.md`.

## Visibility

`SpscRing` exposes `empty()` and `full()`, but they carry **role-specific** guarantees:

- `empty()` is authoritative only on the **consumer** thread. If the consumer sees `empty()`, no
  element is available *right now*; a producer may make one available the next instant, but the
  consumer never misses an element that was published before its check.
- `full()` is authoritative only on the **producer** thread. If the producer sees `full()`, there
  is no free slot *right now*; the consumer may free one the next instant.
- Observed from the *other* side, both can be momentarily stale: a producer calling `empty()` or a
  consumer calling `full()` may read a value that is already out of date. That staleness is always
  *conservative* — it can only report "more full" / "more empty" than reality, never invent
  capacity or data — so it is safe for diagnostics but must not be used as a cross-thread
  handshake. Cross-thread coordination goes through `try_push`/`try_pop` return values, never
  through `empty()`/`full()`.

In practice each stage uses the return value of its own operation (`try_push` returned false ⇒ act
on backpressure; `try_pop` returned false ⇒ nothing to do) rather than polling the opposite side's
view.

## Backpressure

The queue is **bounded** and **never blocks**. `try_push` returns `false` (leaving the queue
unchanged) when the ring is full, and `try_pop` returns `false` when it is empty. The queue makes
no policy decision about what to do next — the *caller* owns the backpressure policy. The three
sane policies for a producer that hits a full queue are:

| Policy            | Producer does                              | When it is appropriate                                  |
| ----------------- | ------------------------------------------ | ------------------------------------------------------- |
| **Spin / yield**  | retry `try_push`, `std::this_thread::yield`| lossless hand-off; the consumer is expected to catch up |
| **Drop**          | discard the item, count the drop           | stale-tolerant data (e.g. throwaway market-data ticks)  |
| **Block upstream**| stop reading its own input until space     | end-to-end flow control back to the source              |

The M26 pipeline will use the **spin/yield (lossless)** policy on the inbound command queue —
orders must not be dropped — and may use **drop-with-counter** on a market-data fan-out where
freshness beats completeness. Capacity sizing is the tuning knob: a larger `Capacity` absorbs
bigger bursts at the cost of memory and worse cache behavior; it does not change correctness.
Because the queue op itself is wait-free, any spinning is strictly application-level and is
measured/bounded by the caller, not hidden inside the queue.

## Shutdown and lifecycle assumptions

The queue has no built-in "close" state; clean shutdown is a property of the *stage pair* around
it, and the M26 pipeline must honor these assumptions:

1. **Lifetime brackets both threads.** The `SpscRing` must outlive both the producer and the
   consumer. Construction happens-before either thread starts; destruction happens-after both have
   been `join()`ed. Destroying the ring while either side may still touch it is undefined behavior
   — the queue does not, and is not meant to, guard its own teardown.
2. **Drain-then-stop.** Shutdown is signalled out-of-band (e.g. an `atomic<bool> stop` flag or a
   sentinel/poison value pushed as the last item), never by destroying the queue under a running
   thread. The consumer's loop is "while not stopped *or* not empty: try to pop", so it drains
   every already-published element before exiting. This guarantees no accepted command is silently
   lost at shutdown.
3. **One-shot, single-direction.** A given ring carries one stream in one direction for the life of
   the pipeline. It is not reset and reused across runs; a fresh pipeline constructs fresh rings.

These are assumptions the *pipeline* enforces; the M25 deliverable is the documented contract plus
the tests that demonstrate lossless drain and backpressure. The pipeline wiring itself is M26.

## False sharing

`head_` and `tail_` are each `alignas(64)` so the producer's hot store to `tail_` and the
consumer's hot store to `head_` do not sit on the same cache line and ping-pong it between cores.
Without the padding, every push would invalidate the cache line the consumer reads `head_` from
(and vice versa), turning two independent cursors into one contended line. The padding trades a
little memory for avoiding that cross-core coherence traffic. (This is a structural choice; no
latency delta is *claimed* here — see Limits.)

## Limits (honest)

- **SPSC only.** One producer thread, one consumer thread. Not MPMC, not a general container.
  Concurrent producers or concurrent consumers are undefined behavior by contract.
- **Bounded, fixed capacity.** No growth; `try_push` returns false when full and the caller decides
  the backpressure policy (spin, drop, or block — see *Backpressure*).
- **Wait-free per operation — and justified, not asserted.** Each `try_push`/`try_pop` is a bounded
  number of atomic loads/stores with no lock and no CAS retry loop; the structural argument is in
  `memory_ordering.md` → *Wait-freedom, by construction*. A caller that *spins* on `full()`/
  `empty()` is doing application-level backpressure, which is separate from the queue op, so the
  *system* is not claimed to be wait-free.
- `T` must be default-constructible (slots are default-initialized); a move-only `T` is not a goal
  for this primitive.
- **Statically reasoned, dynamically spot-checked.** Correctness is argued against the C++ memory
  model and exercised by sustained stress/backpressure tests under `make check` and `make asan`.
  ASan/UBSan do not detect data races; ThreadSanitizer coverage for the concurrent tests is a
  dedicated milestone (M27).
- This is a correctness-first primitive; **no latency/throughput numbers are claimed here.** Any
  such numbers will come only from the committed benchmark harness with full metadata.

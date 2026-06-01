# Concurrency model

> Phase III, M24 — initial section. This documents the first concurrency primitive, the bounded
> SPSC ring buffer (`include/qsl/concurrency/spsc_ring.hpp`). The ownership model, stress/
> backpressure evidence, and the threaded pipeline are expanded in M25/M26.

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
pair per direction and the operations wait-free.

`SpscRing<T, Capacity>` is therefore intentionally **not** a general concurrent container:
concurrent producers, or concurrent consumers, are undefined behavior.

## Ownership

| State          | Written by | Read by              |
| -------------- | ---------- | -------------------- |
| `tail_` (write cursor) | producer   | producer (relaxed), consumer (acquire) |
| `head_` (read cursor)  | consumer   | consumer (relaxed), producer (acquire) |
| `buffer_[tail_]`       | producer   | consumer (after the producer publishes `tail_`) |

A slot is owned by the producer until `tail_` is published with `release`; after that the
consumer owns it until `head_` is published with `release`. No slot is ever written by one thread
while read by the other.

## Memory ordering

One acquire/release pair per direction is sufficient; the release store publishes the slot write
and the matching acquire load establishes the happens-before that makes it visible.

| Step | Operation | Ordering | Why |
| ---- | --------- | -------- | --- |
| producer | load `tail_` | relaxed | only the producer writes it; no cross-thread sync needed |
| producer | load `head_` (full check) | acquire | observe the consumer freeing slots |
| producer | write `buffer_[tail_]` | plain | slot is producer-owned until published |
| producer | store `tail_` | release | publishes the slot write to the consumer |
| consumer | load `head_` | relaxed | only the consumer writes it |
| consumer | load `tail_` (empty check) | acquire | observe the producer's published slot |
| consumer | read `buffer_[head_]` | plain | slot is consumer-owned once observed non-empty |
| consumer | store `head_` | release | publishes the freed slot to the producer |

One spare slot (`kSlots = Capacity + 1`) distinguishes full from empty without a separate count,
so `capacity()` usable elements fit.

## False sharing

`head_` and `tail_` are each `alignas(64)` so the producer's hot store to `tail_` and the
consumer's hot store to `head_` do not sit on the same cache line and ping-pong it between cores.

## Limits (honest)

- **SPSC only.** One producer thread, one consumer thread. Not MPMC, not a general container.
- **Bounded, fixed capacity.** No growth; `try_push` returns false when full (the caller decides
  the backpressure policy — spinning, dropping, or blocking is the caller's concern, documented
  further in M25/M26).
- **Wait-free per operation, not "wait-free system".** Each `try_push`/`try_pop` is a bounded
  number of atomic loads/stores with no lock and no CAS retry loop. A caller that *spins* on
  `full()`/`empty()` is doing application-level backpressure, which is separate from the queue op.
- `T` must be default-constructible (slots are default-initialized); move-only `T` is not a goal
  for M24.
- This is a correctness-first primitive; no latency/throughput numbers are claimed here. Any such
  numbers will come only from the committed benchmark harness with full metadata.

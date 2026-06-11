# Memory ordering of the SPSC ring

> Phase III, M25. This is the C++ memory-model justification for
> `include/qsl/concurrency/spsc_ring.hpp`. It is the companion to
> [`concurrency_model.md`](concurrency_model.md), which covers the higher-level ownership,
> backpressure, and shutdown model. Read that first for *why* the queue is SPSC; read this for
> *why the atomics are correct*.

The claims here are checked empirically by
[`tests/concurrency/test_spsc_stress.cpp`](../tests/concurrency/test_spsc_stress.cpp) and
[`tests/concurrency/test_backpressure.cpp`](../tests/concurrency/test_backpressure.cpp). Dynamic
data-race detection (ThreadSanitizer) was added in M27, and deterministic scheduling perturbation
plus opt-in repeated stress were added in M33; see
[Limits](#limits-and-what-this-does-not-prove).

## The shared state

`SpscRing<T, Capacity>` shares exactly two atomic words between the two threads:

| Field   | Meaning                       | Written by | Storage                       |
| ------- | ----------------------------- | ---------- | ----------------------------- |
| `tail_` | next slot the producer writes | producer   | `alignas(64) atomic<size_t>`  |
| `head_` | next slot the consumer reads  | consumer   | `alignas(64) atomic<size_t>`  |

The element storage `buffer_[kSlots]` (`kSlots = Capacity + 1`) is **not** atomic. It is accessed
with plain loads/stores, and the two indices are the only synchronization. Each index is
single-writer: `tail_` is written only by the producer, `head_` only by the consumer. That
single-writer property is what makes "load my own cursor relaxed" sound.

## Per-operation ordering

Each side performs a fixed sequence of accesses. There are no loops and no compare-and-swap.

### Producer — `try_push` / `emplace`

| Step | Access                       | Order     | Justification                                                          |
| ---- | ---------------------------- | --------- | ---------------------------------------------------------------------- |
| 1    | load `tail_`                 | `relaxed` | producer is the only writer of `tail_`; no cross-thread edge needed    |
| 2    | load `head_` (full check)    | `acquire` | synchronizes-with the consumer's `release` store of `head_` (step C4)  |
| 3    | write `buffer_[tail]`        | plain     | the slot is producer-owned until step 4 publishes it                   |
| 4    | store `tail_ = next(tail)`   | `release` | publishes step 3's write to the consumer                               |

### Consumer — `try_pop`

| Step | Access                       | Order     | Justification                                                          |
| ---- | ---------------------------- | --------- | ---------------------------------------------------------------------- |
| C1   | load `head_`                 | `relaxed` | consumer is the only writer of `head_`; no cross-thread edge needed    |
| C2   | load `tail_` (empty check)   | `acquire` | synchronizes-with the producer's `release` store of `tail_` (step 4)   |
| C3   | read `buffer_[head]`         | plain     | the slot is consumer-owned once C2 observed it non-empty               |
| C4   | store `head_ = next(head)`   | `release` | publishes the freed slot to the producer                               |

The observational helpers `empty()` and `full()` load both indices with `acquire`. They are
intentionally conservative and are only authoritative when called from the thread that owns the
relevant cursor (see `concurrency_model.md` → *Visibility*).

M44's false-sharing benchmark uses the same cursor ownership and release/acquire observation
pattern in a benchmark-only packed-vs-padded control. That study does not change the correctness
argument here: padding can reduce cache-line contention, but the happens-before edges still come
from the release/acquire pairs on `tail_` and `head_`, not from cache-line placement.

## Why two `acquire` loads, not one

Both directions need a release/acquire pair, because the buffer is shared in **both** directions:
the producer publishes *data* into a slot, and the consumer publishes *the freeing* of that slot.

### Direction 1: producer publishes data (step 4 → C2)

```text
producer:  write buffer_[t]  ─sequenced-before→  store tail_ (release)
                                                       │ synchronizes-with
                                                       ▼
consumer:  load tail_ (acquire)  ─sequenced-before→  read buffer_[t]
```

Because the `release` store of `tail_` *synchronizes-with* the `acquire` load that observes it,
everything sequenced-before the store (the slot write) *happens-before* everything sequenced-after
the load (the slot read). The consumer therefore never reads a slot before the producer's write to
it is visible. This is the edge that makes step C1's relaxed load of `head_` harmless: the data
visibility rides on `tail_`, not on `head_`.

### Direction 2: consumer publishes the freed slot (C4 → step 2)

```text
consumer:  read buffer_[h]  ─sequenced-before→  store head_ (release)
                                                      │ synchronizes-with
                                                      ▼
producer:  load head_ (acquire)  ─sequenced-before→  write buffer_[h] (a later cycle)
```

When the producer's full check (step 2) observes that `head_` has advanced past the slot it is
about to reuse, the consumer's read of that slot (C3) *happens-before* the producer's overwrite
(step 3 of a later push). Without the `acquire` here, the producer could overwrite a slot the
consumer has not finished reading — a data race even though the index arithmetic looks fine. This
is why step 2 is `acquire` and not `relaxed`.

The one spare slot (`kSlots = Capacity + 1`) guarantees the producer can only catch up to, never
pass, the consumer, so "the slot I am about to reuse" is always the slot the consumer most
recently freed — exactly the slot direction 2 protects.

## Why `relaxed` is enough for the own-cursor loads

`tail_` has a single writer (the producer) and `head_` has a single writer (the consumer). A
thread loading its own cursor is reading a value only it could have changed, in program order, so
no inter-thread ordering is required — `relaxed` suffices and avoids a needless fence. The
cross-thread edges are carried entirely by the four `acquire`/`release` accesses above.

## Why not `seq_cst`

`memory_order_seq_cst` would add a single total order across *all* sequentially-consistent atomics.
This queue needs no agreement on the relative order of independent atomics: each direction is a
one-way publication validated by its own release/acquire pair, and there is no third observer that
must see `head_` and `tail_` updates interleaved in a particular global order. Release/acquire is
the weakest ordering that is sufficient, so it is the one used. On most ISAs it also avoids the
full barrier that `seq_cst` stores imply.

## Wait-freedom, by construction

The header describes `try_push`/`try_pop` as **wait-free per operation** for payload types whose
copy/move assignment is itself bounded and non-blocking. The justification is structural, not
benchmarked:

- Each operation executes a *fixed, bounded* number of steps in the queue protocol itself — two
  atomic loads, one branch, one plain memory access, one atomic store — regardless of what the
  other thread is doing.
- There is **no loop** and **no CAS retry** inside either operation, so there is no execution in
  which a thread is starved or must retry because of contention.

For `T` with trivial or otherwise constant-time, non-blocking copy/move assignment, that bounded
protocol step count is exactly *population-oblivious wait-freedom*, the strongest progress class;
lock-freedom follows because wait-freedom implies it. This is a claim about the *queue protocol*
plus the payload operation's own boundedness, and it is what "lock-free"/"wait-free" mean in the
header and in `concurrency_model.md`.

It is **not** a claim that a *program* using the queue is wait-free. A caller that spins on a full
or empty queue (`while (!ring.try_push(x)) yield();`) is doing application-level backpressure; that
spin is the caller's policy, outside the queue operation, and is covered under *backpressure* in
`concurrency_model.md`.

## Limits and what this does not prove

- **SPSC only.** The reasoning above assumes exactly one producer thread and one consumer thread.
  With concurrent producers or concurrent consumers the single-writer-per-index premise breaks and
  the relaxed own-cursor loads are no longer sound. This is undefined behavior by contract, not a
  supported mode.
- **Static argument, not a model-checked proof.** The happens-before reasoning is by inspection
  against the C++ memory model; it is not a machine-checked proof (e.g. CDSChecker/`herd`).
- **Empirical evidence is necessary, not sufficient.** The concurrency tests run sustained
  producer/consumer flows and assert strict FIFO, no loss, and a final-empty queue, which would
  surface a missing release/acquire as dropped or reordered values on a weakly-ordered machine.
  They run clean under the ASan/UBSan preset (`make asan`), but ASan/UBSan do not detect data
  races. Dynamic race detection via ThreadSanitizer is a separate, dedicated step from **M27**;
  M33 adds deterministic scheduling perturbation and an opt-in repeated stress command
  (`make concurrency-stress`). These are stronger evidence, not exhaustive proof.
- **No latency/throughput numbers here.** Any such number comes only from the committed benchmark
  harness with full metadata, never from this document.

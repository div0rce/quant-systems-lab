# Concurrency model

> Phase III, M24-M25. This documents the first concurrency primitive, the bounded SPSC ring buffer
> (`include/qsl/concurrency/spsc_ring.hpp`): why it is single-producer/single-consumer, who owns
> what, how producer and consumer observe each other, the backpressure and shutdown contract the
> later pipeline depends on, and the honest limits. The C++ memory-model details (per-step
> acquire/release reasoning and the happens-before proof) live in
> [`memory_ordering.md`](memory_ordering.md). The threaded pipeline that consumes this queue is
> realized in M26, see [Realized pipeline (M26)](#realized-pipeline-m26).
>
> The behavioral claims here are exercised by
> [`tests/concurrency/test_spsc_stress.cpp`](../tests/concurrency/test_spsc_stress.cpp) and
> [`tests/concurrency/test_backpressure.cpp`](../tests/concurrency/test_backpressure.cpp). The
> threaded pipeline also has deterministic scheduling-perturbation coverage in
> [`tests/concurrency/test_pipeline.cpp`](../tests/concurrency/test_pipeline.cpp).

## Why SPSC, not MPMC

The threaded pipeline (M26, realized in `include/qsl/concurrency/pipeline.hpp`) is a chain of
single-threaded stages connected by hand-off queues:

```text
input thread  --[inbound queue]-->  engine thread  --[outbound queue]-->  publisher/log thread
```

Each queue has **exactly one producer and one consumer**. That is the defining condition for a
single-producer/single-consumer (SPSC) queue, so an MPMC queue would add compare-and-swap
contention, retry loops, and ABA/hazard concerns for a problem we do not have. SPSC lets each
index be owned by exactly one thread, which keeps the synchronization to a single acquire/release
pair per direction. The queue protocol is wait-free for payload types whose copy/move assignment
is bounded and non-blocking (see `memory_ordering.md`).

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
  *conservative*, it can only report "more full" / "more empty" than reality, never invent
  capacity or data, so it is safe for diagnostics but must not be used as a cross-thread
  handshake. Cross-thread coordination goes through `try_push`/`try_pop` return values, never
  through `empty()`/`full()`.

In practice each stage uses the return value of its own operation (`try_push` returned false ⇒ act
on backpressure; `try_pop` returned false ⇒ nothing to do) rather than polling the opposite side's
view.

## Backpressure

The queue is **bounded** and **never blocks**. `try_push` returns `false` (leaving the queue
unchanged) when the ring is full, and `try_pop` returns `false` when it is empty. The queue makes
no policy decision about what to do next, the *caller* owns the backpressure policy. The three
sane policies for a producer that hits a full queue are:

| Policy            | Producer does                              | When it is appropriate                                  |
| ----------------- | ------------------------------------------ | ------------------------------------------------------- |
| **Spin / yield**  | retry `try_push`, `std::this_thread::yield`| lossless hand-off; the consumer is expected to catch up |
| **Drop**          | discard the item, count the drop           | stale-tolerant data (e.g. throwaway market-data ticks)  |
| **Block upstream**| stop reading its own input until space     | end-to-end flow control back to the source              |

The realized M26 pipeline uses the **spin/yield (lossless)** policy on **both** hand-off queues:
orders must not be dropped on the inbound command queue, and event-log/feed records must not be
silently dropped on the outbound queue. The **drop-with-counter** policy remains available for a
stale-tolerant market-data fan-out (where freshness beats completeness) but the prototype does not
use it. Capacity sizing is the tuning knob: a larger `Capacity` absorbs
bigger bursts at the cost of memory and worse cache behavior; it does not change correctness.
Because the queue op itself is wait-free for payload types with bounded, non-blocking
copy/move assignment, any spinning is strictly application-level and is measured/bounded by the
caller, not hidden inside the queue.

## Shutdown and lifecycle assumptions

The queue has no built-in "close" state; clean shutdown is a property of the *stage pair* around
it, and the M26 pipeline must honor these assumptions:

1. **Lifetime brackets both threads.** The `SpscRing` must outlive both the producer and the
   consumer. Construction happens-before either thread starts; destruction happens-after both have
   been `join()`ed. Destroying the ring while either side may still touch it is undefined behavior, the queue does not, and is not meant to, guard its own teardown.
2. **Drain-then-stop.** Shutdown is signalled out-of-band (e.g. an `atomic<bool> stop` flag or a
   sentinel/poison value pushed as the last item), never by destroying the queue under a running
   thread. The consumer's loop is "while not stopped *or* not empty: try to pop", so it drains
   every already-published element before exiting. This guarantees no accepted command is silently
   lost at shutdown.
3. **One-shot, single-direction.** A given ring carries one stream in one direction for the life of
   the pipeline. It is not reset and reused across runs; a fresh pipeline constructs fresh rings.

These are assumptions the *pipeline* enforces; the M25 deliverable is the documented contract plus
the tests that demonstrate lossless drain and backpressure. The pipeline wiring that enforces them
is realized in M26 (see *Realized pipeline* below).

## False sharing

`head_` and `tail_` are each `alignas(64)` so the producer's hot store to `tail_` and the
consumer's hot store to `head_` do not sit on the same cache line and ping-pong it between cores.
Without the padding, every push would invalidate the cache line the consumer reads `head_` from
(and vice versa), turning two independent cursors into one contended line. The padding trades a
little memory for avoiding that cross-core coherence traffic.

M44 adds a benchmark-only control layout for this point: `make false-sharing-study` runs
`qsl-bench false-sharing`, which compares packed queue indices against cache-line-padded indices
under the same producer-owned `tail` / consumer-owned `head` access pattern. The benchmark-only
padded control separates indices by 128 bytes so it stays separated on Apple Silicon-style
128-byte coherency lines. The production `SpscRing` layout is not changed by that study and still
uses `alignas(64)`, so the artifact should not be read as validating production cursor separation
on hosts with wider coherency lines. It measures contention shape and records the
hardware/compiler/build metadata in `results/false_sharing_study.txt`; the result is
host-dependent cache-line evidence, not a production throughput claim.

## Limits (honest)

- **SPSC only.** One producer thread, one consumer thread. Not MPMC, not a general container.
  Concurrent producers or concurrent consumers are undefined behavior by contract.
- **Bounded, fixed capacity.** No growth; `try_push` returns false when full and the caller decides
  the backpressure policy (spin, drop, or block, see *Backpressure*).
- **Wait-free per operation, and qualified.** Each `try_push`/`try_pop` has a bounded atomic
  protocol with no lock and no CAS retry loop; for payload types with bounded, non-blocking
  copy/move assignment, the operation is wait-free end-to-end. The structural argument is in
  `memory_ordering.md` → *Wait-freedom, by construction*. A caller that *spins* on `full()`/
  `empty()` is doing application-level backpressure, which is separate from the queue op, so the
  *system* is not claimed to be wait-free.
- `T` must be default-constructible (slots are default-initialized); a move-only `T` is not a goal
  for this primitive.
- **Statically reasoned, dynamically checked.** Correctness is argued against the C++ memory
  model and exercised by sustained stress/backpressure tests under `make check` and `make asan`.
  ASan/UBSan do not detect data races, so the concurrent tests are also run under ThreadSanitizer
  (`make tsan`, M27, see *ThreadSanitizer* below).
- This is a correctness-first primitive; **no latency/throughput numbers are claimed here.** Any
  such numbers will come only from the committed benchmark harness with full metadata. The M44
  false-sharing study is benchmark-only and must not be generalized beyond its recorded host.

## Realized pipeline (M26)

M26 wires the SPSC ring into the prototype it was designed for: the header-only
`ThreadedPipeline<InboundCapacity, OutboundCapacity>` in
[`include/qsl/concurrency/pipeline.hpp`](../include/qsl/concurrency/pipeline.hpp). The behavioral
evidence is [`tests/concurrency/test_pipeline.cpp`](../tests/concurrency/test_pipeline.cpp).

```text
input thread --[inbound SpscRing<Command>]--> engine thread --[outbound SpscRing<ProcessedCommand>]--> publisher/log thread
```

### Stages and ownership transfer

| Stage         | Thread owns exclusively              | Consumes                     | Produces                              |
| ------------- | ------------------------------------ | ---------------------------- | ------------------------------------- |
| Input         | the command source (`vector<Command>`) |, | pushes `Command`s onto inbound        |
| Engine        | the `MatchingEngine` + `OrderGateway`  | pops `Command`s from inbound | pushes `ProcessedCommand`s onto outbound |
| Publisher/log | the downstream `OutputSink`            | pops from outbound           | side effects (log append, feed, counters) |

Ownership transfers *with the value*: once the engine thread pushes a `ProcessedCommand` it never
touches it again, and the publisher/log thread owns it until the next pop. Crucially, the **engine
thread is the only thread that ever touches the engine**, so the deterministic single-threaded
matching semantics are unchanged, the boundary is a value hand-off, never shared mutable state.

### Why the downstream stage is engine-independent

The M6 `MarketDataPublisher` derives top-of-book by *reading the engine*. Running it on the
publisher/log thread would read the engine concurrently with the engine thread mutating it, a data
race. The prototype therefore has the downstream `OutputSink` consume only self-contained
`ProcessedCommand` records (the command, its accept/reject outcome, and the events it produced) and
never touch the engine. A downstream consumer that genuinely needs top-of-book would read it from
the events, or from a snapshot captured on the engine thread and shipped in the record. This is what
makes "a lagging publisher cannot corrupt engine state" *structurally* true, not merely tested.

### Backpressure and shutdown

- **Lossless both directions.** Both queues use the spin/yield policy: the input thread spins on a
  full inbound queue (an order is never dropped) and the engine thread spins on a full outbound
  queue (an event-log/feed record is never silently dropped). The run reports how many times each
  side spun (`inbound_backpressure_spins` / `outbound_backpressure_spins`).
- **Drain-then-stop.** Each upstream stage publishes an `atomic<bool>` done-flag with `release`
  after its last push; the downstream stage, on an empty queue, checks the flag with `acquire` and
  if set performs one final drain before exiting. Nothing queued at shutdown is lost.
- **Lifetime bracket.** The two rings live on `run()`'s stack; `run()` joins all three threads
  before returning, so each ring outlives both its producer and its consumer (the SPSC contract).

### What the tests prove

For GTC synthetic flows, enriched property flows, several seeds, and queue capacities from `2` up to
`4096`, the threaded run produces the **same final snapshot and the same ordered event stream as a
single-threaded reference**. Capacity and thread timing change only backpressure, never the result.
The event-log integrity test additionally replays the *concurrently written* command log into a
fresh engine and shows it reconstructs the identical snapshot (`pipeline state == log replay ==
single-threaded reference`). Backpressure tests confirm a full inbound queue and a lagging publisher
each apply real backpressure (spin counts > 0) while staying lossless.

M33 adds deterministic scheduling perturbation: tests can ask the input, engine, and output stages
to yield at fixed step intervals (`PipelinePerturbation`). This broadens the set of executed
interleavings without relying on sleeps or timing-sensitive assertions.

### Limits (honest)

This is a **correctness-first prototype of a concurrency boundary**, not a latency exercise: no
matching-latency or throughput number is claimed (none is measured here). It is SPSC point-to-point,
not MPMC and not a fan-out/fan-in topology. `make check` and `make asan` exercise the threaded paths,
but ASan/UBSan do not detect data races, dynamic race detection runs under ThreadSanitizer
(`make tsan`, M27, see below).

## Advanced validation (M33)

M33 adds two validation layers:

- **Deterministic scheduling perturbation** in the normal pipeline test. The perturbation hook
  yields after configured stage-step counts, so the same command stream is checked under different
  input/engine/output pacing patterns while remaining deterministic and non-flaky.
- **Opt-in long-run repetition** via:

```bash
make concurrency-stress
```

That target repeats the `concurrency`-labelled tests outside normal CI. It is intentionally
developer-invoked because repeated stress runs cost time and still do not prove every possible
interleaving. Useful knobs:

```bash
QSL_CONCURRENCY_STRESS_LOOPS=100 make concurrency-stress
QSL_CONCURRENCY_STRESS_PRESET=tsan QSL_CONCURRENCY_STRESS_LOOPS=10 make concurrency-stress
```

Use the `tsan` preset only where ThreadSanitizer is supported by the local toolchain. These runs
increase schedule coverage and can expose rare races or shutdown bugs, but they remain empirical
evidence over executed schedules.

## ThreadSanitizer (M27)

ThreadSanitizer (TSan) is the dynamic complement to the static memory-ordering argument and the
ASan/UBSan build: it instruments memory accesses and synchronization at run time and reports any
**data race**, two threads touching the same location with no happens-before edge between them and
at least one write. ASan/UBSan cannot see races, so TSan is the tool that actually exercises the
acquire/release reasoning in [`memory_ordering.md`](memory_ordering.md) on real schedules.

Run it with:

```bash
make tsan            # equivalently: ctest --preset tsan -L concurrency
```

The `tsan` preset builds with `-fsanitize=thread`, a *separate* build from `asan`, because the two
sanitizers instrument memory incompatibly and cannot be combined (`cmake/Sanitizers.cmake` errors if
both are enabled). It runs the `concurrency`-labelled tests: the SPSC stress and backpressure suites
and the threaded-pipeline suite. Those are the only genuinely multithreaded tests; running TSan over
single-threaded tests would add nothing.

**TSan is a correctness gate, not a performance tool.** It imposes large time/memory overhead and
perturbs scheduling, so **no benchmark or latency number is ever collected under TSan**, measured
numbers come only from the committed benchmark harness. A green TSan run means "no data race was
observed on the schedules that executed": it is dynamic evidence that *strengthens* the static
happens-before proof, not an exhaustive proof over all possible interleavings.

CI runs a dedicated `thread-sanitizer` job on every PR (where the toolchain supports it), so the
concurrent code is race-checked continuously rather than only locally.

# Architecture

> Placeholder — will be expanded as components are built.

## Overview

Quant Systems Lab is a deterministic C++20 exchange simulator.

```text
ClientCommand -> Risk -> MatchingEngine -> EngineEvents -> Publisher
```

## Components

- **Core types** — integer ticks, order IDs, sequence numbers
- **Binary protocol** — fixed-width encode/decode (M2)
- **Order book** — price-time priority (M3)
- **Matching engine** — multi-symbol sequencing (M4)
- **Risk** — deterministic pre-trade checks (M5)
- **Market data** — trade/top-of-book publisher (M6)
- **Event log** — append-only replayable log (M7)
- **Replay/recovery** — rebuild state from log (M8)
- **TCP gateway** — binary order gateway (M9)
- **Benchmarks** — reproducible performance measurement (M11)

## Core domain model (M1)

Defined in `include/qsl/core/`.

### Integer price ticks

Prices are integer ticks (`Price = std::int64_t`), never floating point. A tick
scale maps display prices to integers: at `kTickScale = 100`, `$123.45` is stored
as `12345`. This keeps arithmetic exact and matching deterministic — floating-point
rounding would make fills and price-time priority non-reproducible.

Other domain aliases: `SymbolId` (u32), `Quantity` (u32), `OrderId` (u64),
`SeqNo` (u64).

### Deterministic (logical) time

`LogicalClock` (`clock.hpp`) produces a monotonic `Timestamp` (u64) that is
independent of wall-clock time. Core engine paths use logical time only, so a
recorded command stream replays to an identical state. Wall-clock time is allowed
only at the gateway boundary and benchmark layer, never inside matching.

### Enums and validation

`Side`, `OrderType`, and `TimeInForce` are fixed-width enums with `is_valid` and
`to_string` helpers. `RejectReason` plus the lightweight `Result` type
(`result.hpp`) give structured, deterministic rejection of invalid input;
`invariants.hpp` holds the domain validity predicates.

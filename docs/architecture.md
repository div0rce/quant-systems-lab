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

## Matching engine (M4)

`MatchingEngine` (`include/qsl/engine/matching_engine.hpp`) wraps the single-symbol
`OrderBook` into a multi-symbol engine.

- **Symbol registry** — `SymbolRegistry` interns external symbol names to compact
  `SymbolId`s assigned in registration order. The engine holds one `OrderBook` per
  registered symbol in a `std::map<SymbolId, OrderBook>` (ordered, for deterministic
  snapshot iteration).
- **Command application** — `new_limit` / `new_market` / `cancel` / `modify` route to the
  symbol's book. A command for an unregistered symbol (or a cancel/modify of an unknown
  order) is a no-op here; structured rejection is the risk layer's job (M5).
- **Active order IDs** — a resting `OrderId` is unique within each symbol book. Duplicate
  active IDs are ignored in M4 before any acceptance event or sequence number is emitted
  so the book's locator index cannot be corrupted. M5 turns this condition into a
  structured `DuplicateOrderId` rejection.
- **Event stream** — commands emit `EngineEvent`, a `std::variant` of `OrderAccepted`,
  `OrderCanceled`, `OrderModified`, and `TradeEvent` (`engine/events.hpp`). A new order
  emits `OrderAccepted` followed by a `TradeEvent` per fill. `OrderRejected` (M5) and
  `BookUpdate` (M6) are added later.
- **Sequencing** — every emitted event carries a `SeqNo` from a single monotonic counter,
  so event sequence numbers are strictly increasing across all symbols and commands.
- **Snapshot** — `snapshot()` returns a deterministic `EngineSnapshot` (last sequence
  number plus per-symbol best bid/ask and resting order count, ordered by `SymbolId`) for
  replay-equivalence comparison. M8 extends it with per-level aggregates and the trade
  sequence.

The engine has no wall-clock dependence; ordering and sequencing are logical.

## Risk checks and in-process gateway (M5)

The `OrderGateway` (`include/qsl/gateway/order_gateway.hpp`) is the risk boundary in front
of the engine:

```text
ClientCommand -> OrderGateway (risk) -> MatchingEngine -> EngineEvents
```

- **Value checks** (`engine/risk.hpp`, `RiskConfig` + `check_limit`/`check_market`) are pure
  and deterministic: invalid side, invalid price tick (price must be positive), invalid
  quantity, max order quantity, and max notional. The notional check is overflow-safe —
  it compares `quantity > max_notional / price` rather than computing `price * quantity`.
- **Identity checks** live in the gateway, which knows engine state: an unregistered symbol
  rejects with `UnknownSymbol`; a new order whose id is already **active** (resting) rejects
  with `DuplicateOrderId`; a cancel/modify of an id that is not resting rejects with
  `UnknownOrder`. "Duplicate" and "unknown" are thus defined by current engine state
  (`MatchingEngine::has_symbol` / `contains`), consistent with the engine's no-op on a
  duplicate active id. Completed-order ids are not retained, so they may be reused.
- **Structured result** — every submission returns a `GatewayResult{accepted, reason,
  events}`. A rejection carries a `RejectReason` and no events and never reaches the engine
  (so the engine's sequence counter and state are untouched); an acceptance carries the
  engine's resulting event stream. Rejections are intentionally *not* part of the engine's
  sequenced event stream because they do not mutate engine state (which keeps replay clean).

Checks run in a fixed order so the returned reason is deterministic when multiple would
apply: unknown symbol, duplicate id, then value checks (side, price, quantity, max
quantity, max notional).

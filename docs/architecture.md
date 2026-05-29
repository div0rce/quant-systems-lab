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

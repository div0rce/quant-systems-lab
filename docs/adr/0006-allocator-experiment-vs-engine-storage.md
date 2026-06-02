# ADR 0006: Allocator Experiment And Engine Storage Are Separate Questions

## Status

Accepted

## Context

M28 implemented a fixed-capacity raw-storage `OrderPool` for `engine::Order` and benchmarked
allocator acquire/release against `new`/`delete`. The pool now constructs objects on acquire and
destroys them on release/reset/destructor, preserving per-acquire object lifetime.

That experiment did not integrate the pool into `OrderBook` or the matching engine. The existing
order-book storage architecture remains unchanged.

## Decision

M28 evidence is allocator evidence only. It must not be described as engine-storage evidence or as
an end-to-end matching-engine improvement.

Pool-backed order-book storage integration is a separate roadmap item (M32). It should be evaluated
against engine-level workloads, replay impact, cache locality, and alternatives such as arenas,
intrusive containers, and flat containers.

## Consequences

The repo can discuss allocator mechanics from M28, but resume/docs claims about order-book memory
architecture require M32-style integration and measured evidence. A negative M32 result is
acceptable and should be documented honestly.

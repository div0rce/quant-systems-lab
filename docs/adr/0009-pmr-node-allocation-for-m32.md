# ADR 0009: PMR Node Allocation For M32

## Status

Accepted

## Context

M28 implemented `OrderPool<Capacity>`, a raw-storage pool for bare `engine::Order` objects. The
current order book, however, uses `std::list<Order>` for FIFO price levels and `std::map` /
`std::unordered_map` for levels and locators. These containers allocate implementation-defined
nodes, not bare `engine::Order` objects.

Forcing `OrderPool<Capacity>` directly into this design would require replacing container node
ownership with custom or intrusive nodes. That is a larger storage-architecture redesign, not the
scoped M32 experiment.

## Decision

M32 uses `std::pmr::unsynchronized_pool_resource` to back order-book container node allocation.
`OrderBook::Storage::Baseline` keeps the default `new_delete_resource`, while
`OrderBook::Storage::Pooled` routes the book's PMR list/map/unordered-map allocations through a
per-book pool resource.

Direct intrusive/custom-node `OrderPool<Capacity>` order-book storage is deferred and tracked in
issue #95.

## Consequences

M32 is reversible, small, and measurable against engine-level workloads without changing matching
semantics. It evaluates whether pooled node allocation helps the existing map/list order-book
architecture.

It does not prove that M28's raw `OrderPool` improves the matching engine, and it does not settle
the future intrusive/custom-node design question.

# Matching Rules

Single-symbol, deterministic, price-time-priority limit order book. Implemented in
`include/qsl/engine/order_book.hpp` and `src/engine/order_book.cpp`. Prices are integer
ticks; there is no wall-clock dependence (time priority is queue position).

## Book structure

- Bids are ordered highest-price-first, asks lowest-price-first (`std::map` with the
  appropriate comparator). The best price on each side is the first entry.
- Each price level is a FIFO queue (`std::list<Order>`); the front is the oldest order
  and therefore has time priority.
- An `OrderId -> {side, price, iterator}` index supports O(1) cancel/modify lookup. It is
  never iterated for matching, so its unordered layout does not affect determinism.

## Crossing

- A **buy** crosses resting asks while `best_ask <= limit`.
- A **sell** crosses resting bids while `best_bid >= limit`.
- A **market** order ignores the limit and crosses best-available liquidity until it is
  filled or the book is depleted. Market orders never rest.

Within a crossing, the engine consumes the best price level front-to-back (time priority),
then moves to the next level. Each fill executes at the **resting maker's price**, so an
aggressor that crosses multiple levels pays each level's price in turn.

## Trades

Each fill emits `Trade{taker_id, maker_id, price, quantity}` where `quantity` is
`min(taker_remaining, maker_remaining)`. A maker fully consumed is removed; a partially
filled maker keeps its place (and thus its priority) with the reduced quantity.

## Time in force

- **GTC**: any unfilled remainder rests in the book.
- **IOC**: any unfilled remainder is discarded (never rests).

## Cancel

`cancel(id)` removes a resting order and returns `false` if the id is not resting.

## Modify and priority

| Change                              | Effect                                              |
|-------------------------------------|-----------------------------------------------------|
| Same price, **smaller** quantity    | Reduced in place — **time priority preserved**      |
| Price change (any)                  | Re-queued at the new level tail — **priority lost**; may cross and trade immediately |
| Quantity **increase**               | Re-queued at the tail — **priority lost**           |
| `new_quantity == 0`                 | Treated as a cancel                                 |

Priority loss is implemented as cancel + re-add, so a repriced order that now crosses the
book trades immediately, exactly as a fresh aggressor would.

## Invariants

- After any operation the book is **never crossed** (`best_bid < best_ask` whenever both
  sides are present), because an aggressor matches until it no longer crosses before any
  remainder rests.
- Executed quantity per fill never exceeds either side's remaining quantity.

Semantic/policy validation (price ticks, max quantity/notional, duplicate ids, unknown
symbols) is **not** done here; that is the risk layer's responsibility (M5). The book
assumes well-formed, validated inputs. In particular, zero-quantity inputs are assumed to
be rejected by the M5 risk layer; at the M3 book layer a zero-quantity order simply
produces no trades and does not rest.

`quantity_at()` reports aggregate resting liquidity using a wider aggregate type
(`QuantityTotal`, 64-bit) so a price level's total never wraps at the per-order `Quantity`
(32-bit) width, even when many orders rest at the same price.

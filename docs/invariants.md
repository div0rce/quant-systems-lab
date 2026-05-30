# Tested Invariants

The properties the deterministic core is expected to hold, and where each is checked. The
randomized checks run the deterministic synthetic flow (`generate_flow`) across multiple
fixed seeds, so failures are reproducible. The sanitizer build (`make asan`, ASan + UBSan)
runs the whole suite — including the fuzz tests — so these checks also catch undefined
behavior and out-of-bounds access.

| # | Invariant | Where tested |
|---|-----------|--------------|
| 1 | No zero/negative quantities (trades and resting levels) | `test_invariants` (randomized flow); `Quantity` is unsigned |
| 2 | Book is never crossed after matching (`best_bid < best_ask`) | `test_invariants` (after every command, all symbols); `test_order_book` |
| 3 | Executed quantity cannot exceed submitted quantity | `test_invariants` (per new order, summed fills ≤ submitted) |
| 4 | A canceled order cannot trade afterward | `test_invariants` (cancel front order, later cross hits the survivor) |
| 5 | A rejected order cannot rest | `test_invariants` (risk-rejected orders are absent from the book); `test_risk_gateway` |
| 6 | `fresh engine + replay(log) == original final state` | `test_invariants` (stress: multiple seeds, 8000 orders); `test_replay` |
| 7 | Sequence numbers are strictly increasing | `test_invariants` (across the whole flow); `test_matching_engine` |
| 8 | Market-data sequence follows the engine event sequence | `test_market_data` / `test_md_feed` (monotonic `md_seq` in engine order) |

## Fuzz / malformed input

`test_fuzz_protocol` feeds tens of thousands of random byte buffers to every decoder
(`decode_header`, the order/session message decoders, and `decode_market_data`) and to the
TCP `Session` (whole buffers and small random chunks). The decoders are non-throwing and
bounds-safe, so they return a deterministic error rather than crashing; the session either
responds or flags disconnect. Under `make asan` this proves there is no out-of-bounds read or
undefined behavior on hostile input.

## Structural guarantees (by construction)

- **No floating-point prices**: `Price` is `std::int64_t` (integer ticks); `test_types`
  statically asserts the type is integral and signed. No float/double appears in the engine.
- **Core engine is wall-clock independent**: matching and sequencing use a logical
  `Timestamp`/`SeqNo`; `std::chrono` appears only at the benchmark layer, never in the engine.

## Running

```bash
make check   # all tests, normal build
make asan    # ASan + UBSan build, same suite (sanitizer-clean)
```

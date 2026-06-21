# FIX-like Text Protocol Adapter

A human-readable `tag=value` text protocol alongside the [binary protocol](binary_protocol.md),
mapping the **same internal message structs**. Implemented in `include/qsl/protocol/fix.hpp` and
`src/protocol/fix.cpp`; tested in `tests/unit/test_fix_protocol.cpp`. This is the optional FIX
adapter tracked by issue #29.

It is **FIX-like**, not a full FIX engine: it implements the genuine FIX framing and the
client→gateway order path, deliberately scoped to what mirrors the binary codec.

## Why it exists

Real venues speak FIX as well as binary protocols. Showing a second, independently-validated wire
format over one internal model demonstrates a clean protocol boundary: the engine does not care
which encoding produced a `NewOrder`. The strongest invariant the tests assert is that a binary
frame and a FIX message for the same order **decode to identical internal structs**.

## Framing

A message is a sequence of `tag=value` fields, each terminated by the **SOH** byte (`0x01`). The
adapter uses the standard FIX envelope:

```text
8=FIX.4.2 | 9=<BodyLength> | 35=<MsgType> | <business fields...> | 10=<CheckSum> |
```

(`|` denotes SOH.)

- **`8` BeginString** must be `FIX.4.2`; anything else is `UnsupportedBeginString`.
- **`9` BodyLength** is the byte count from the field after tag 9 through the SOH before tag 10.
  A mismatch is `BodyLengthMismatch`.
- **`10` CheckSum** is the mod-256 sum of every byte up to the SOH before tag 10, formatted as
  exactly three zero-padded digits. A mismatch is `ChecksumMismatch`.

## Messages

### NewOrderSingle (`35=D`) → `NewOrder`

| Tag | FIX name      | Internal field | Encoding |
|-----|---------------|----------------|----------|
| 34  | MsgSeqNum     | sequence no.   | carried like the binary header `seq_no` (validated, not stored in the body struct) |
| 11  | ClOrdID       | `order_id`     | decimal |
| 55  | Symbol        | `symbol`       | decimal `SymbolId` (see simplifications) |
| 54  | Side          | `side`         | `1`=Buy, `2`=Sell |
| 38  | OrderQty      | `quantity`     | decimal |
| 40  | OrdType       | `type`         | `1`=Market, `2`=Limit |
| 44  | Price         | `price`        | integer ticks (see simplifications) |
| 59  | TimeInForce   | `tif`          | `1`=GTC, `3`=IOC |

### OrderCancelRequest (`35=F`) → `CancelOrder`

| Tag | FIX name     | Internal field | Notes |
|-----|--------------|----------------|-------|
| 34  | MsgSeqNum    | sequence no.   | as above |
| 41  | OrigClOrdID  | `order_id`     | the order being cancelled |
| 11  | ClOrdID      | —              | required by FIX; echoes `order_id` (the cancel request id is not modelled) |
| 55  | Symbol       | `symbol`       | decimal `SymbolId` |

## Deliberate simplifications

These are documented departures from strict FIX, chosen so the adapter stays a deterministic,
lossless map onto the simulator's internal model:

- **Symbol (tag 55) carries the numeric `SymbolId`** in decimal, not a ticker string — the engine
  keys on `SymbolId`, so mapping to a string table would only add a lossy layer.
- **Price (tag 44) carries integer ticks and is always present**, including market orders. The
  project never represents price as a float, and `NewOrder` always has a `price` field; carrying it
  losslessly makes `NewOrder ↔ FIX` a true bijection over the internal struct, exactly like the
  binary codec. (Strict FIX uses a decimal price and forbids tag 44 on market orders.)

## Error model

Decoding is total and deterministic: it never throws, allocates nothing on the decode path (a
fixed field table, `std::from_chars`, `std::string_view`), and reports every malformed input via
`FixError` rather than undefined behavior — mirroring the binary codec's `DecodeError` discipline.

`FixError`: `None`, `Malformed`, `UnsupportedBeginString`, `UnknownMsgType`, `MissingField`,
`InvalidField`, `BodyLengthMismatch`, `ChecksumMismatch`, `InvalidEnumValue`, `OutOfRange`.

## Determinism and testing

`tests/unit/test_fix_protocol.cpp` mirrors the binary codec's required tests and adds FIX-specific
ones:

- round-trip for NewOrderSingle and OrderCancelRequest;
- **cross-codec equivalence**: binary and FIX decode the same order to identical structs across all
  Side × OrdType × TIF combinations;
- a **byte-pinned fixture** (`8=FIX.4.2|9=50|35=D|…|10=164|`) so any change to field order or the
  checksum/body-length computation fails the build;
- rejection of malformed framing, unsupported BeginString, unknown/wrong MsgType, BodyLength
  mismatch, CheckSum mismatch, missing required fields, non-numeric fields, invalid enum codes,
  out-of-range integers, and oversized messages;
- signed/extreme `int64` price and `uint64` id/seq round-trips.

The adapter is also covered by the ASan/UBSan preset (`make asan`), since it parses untrusted text.

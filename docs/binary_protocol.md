# Binary Protocol

Fixed-width binary protocol for the order gateway and (later) market data. Implemented
in `include/qsl/protocol/` and `src/protocol/codec.cpp`.

## Design goals

- Fixed-width messages for predictable parsing.
- **Big-endian (network byte order)** at the protocol boundary.
- **Explicit serialization** via byte shifts (`endian.hpp`) — no `reinterpret_cast`,
  no `memcpy`, no struct overlay, so there is no undefined behavior from layout/aliasing.
- A version field for forward compatibility.
- Deterministic rejection of malformed frames.

Internal engine structs are independent of this wire layout.

## Frame

```text
+------------------- header (16 bytes) -------------------+----- body -----+
| type (u16) | version (u16) | body_len (u32) | seq (u64) | body (body_len)|
+---------------------------------------------------------+----------------+
```

All multi-byte integers are big-endian. `body_len` counts body bytes only (the header
is fixed at 16 bytes). Signed `Price` is transported as its 64-bit two's-complement
bit pattern (`std::int64_t` <-> `std::uint64_t` via `static_cast`, which is well-defined).

- Protocol version: `kProtocolVersion = 1`
- Header size: `kHeaderSize = 16`
- Maximum body length: `kMaxBodyLen = 4096`

## Message types (`MsgType`, u16)

| Value | Name        | Body size | Fields (in order)                                            |
|-------|-------------|-----------|--------------------------------------------------------------|
| 1     | NewOrder    | 27        | order_id u64, symbol u32, price i64, quantity u32, side u8, order_type u8, tif u8 |
| 2     | CancelOrder | 12        | order_id u64, symbol u32                                     |

The registry grows in later milestones (modify, heartbeat, gateway responses, market data).

## Decode errors (`DecodeError`)

Decoding never throws and never reads out of bounds; it returns a deterministic error:

| Error                | Condition                                              |
|----------------------|--------------------------------------------------------|
| `Truncated`          | fewer bytes than the header, or than the declared body |
| `UnsupportedVersion` | header `version` != `kProtocolVersion`                 |
| `UnknownType`        | header `type` not in the registry (or wrong decoder)   |
| `BodyTooLarge`       | declared `body_len` > `kMaxBodyLen`                    |
| `BodyLengthMismatch` | declared `body_len` != the message type's fixed size   |

Header validation (`decode_header`) checks version, type, and max length. Typed decoders
(`decode_new_order`, `decode_cancel_order`) additionally verify the body size and that the
buffer holds the full declared body before parsing.

## Trailing bytes and framing

Typed decoders treat `body_len` as authoritative and parse exactly the declared fixed
body, so a buffer with extra bytes after the body still decodes (the trailing bytes are
ignored rather than treated as an error). Exact-size enforcement and message framing over
a byte stream belong to the TCP/session layer (M9), not the codec.

## Determinism

The wire format is pinned by a byte-fixture test (`tests/unit/test_protocol.cpp`) so any
accidental change to field order or byte order fails the build.

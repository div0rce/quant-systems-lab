# Binary Protocol

> Placeholder — implemented in M2.

## Design goals

- Fixed-width messages for predictable parsing
- Big-endian network byte order at protocol boundary
- Explicit serialization (no struct reinterpret casts)
- Version field for forward compatibility
- Deterministic rejection of malformed frames

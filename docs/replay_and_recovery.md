# Replay and Recovery

## Append-only event log (M7)

Accepted commands and emitted engine events are persisted as framed records in an
append-only log (`include/qsl/replay/event_log.hpp`, `src/replay/event_log.cpp`). The
writer opens the file in binary append mode and only ever appends — it never seeks back or
rewrites existing records.

### Record format

All integers are big-endian (reusing the M2 protocol byte helpers).

```text
+--------------------- header (22 bytes) ----------------------+--- payload ---+--checksum--+
| seq_no u64 | record_type u16 | logical_timestamp u64 | size u32 | payload (size) | u32 |
+--------------------------------------------------------------+---------------+------------+
```

- `record_type`: `Command` (1) or `Event` (2).
- `payload`: the message's serialized bytes, opaque to the log (e.g. a binary-protocol
  command frame). Capped at `kMaxPayload` (1 MiB). The writer rejects records above this
  cap before writing, so a successful append remains readable by this implementation.
- `checksum`: FNV-1a 32-bit over the record's header + payload bytes.

### Reading and failure handling

`read_log(bytes)` decodes records sequentially and never reads out of bounds. It stops at
the first corrupt or truncated record and returns the records read so far plus a
deterministic `LogError`:

| Error             | Condition                                                  |
|-------------------|------------------------------------------------------------|
| `Truncated`       | the buffer ends before a full header, payload, or checksum |
| `BadChecksum`     | the stored checksum does not match the record's bytes      |
| `PayloadTooLarge` | the declared payload size exceeds `kMaxPayload`            |

A buffer that ends exactly on a record boundary reads cleanly (`LogError::None`); a
truncated trailing record is reported as `Truncated` while earlier intact records are still
returned.

`EventLogWriter::append` checks both `fwrite` and `fflush` before reporting success. M7
does not claim `fsync` or durable-to-disk semantics; the guarantee is stdio flush
correctness for the append path.

`apps/qsl-loginspect` is a small CLI that prints a human-readable summary of a log file
(record count, sequence range, command/event counts, and status).

## Replay invariant (M8)

```text
fresh engine + replay(log) == original engine final state
```

### Comparison dimensions

- Best bid/ask per symbol
- Resting order counts
- Aggregate quantity at each price level
- Emitted trade sequence
- Last sequence number

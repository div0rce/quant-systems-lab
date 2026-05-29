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
| `OpenFailed`      | the requested log file cannot be opened or read            |
| `Truncated`       | the buffer ends before a full header, payload, or checksum |
| `BadChecksum`     | the stored checksum does not match the record's bytes      |
| `PayloadTooLarge` | the declared payload size exceeds `kMaxPayload`            |

A buffer that ends exactly on a record boundary reads cleanly (`LogError::None`); a
truncated trailing record is reported as `Truncated` while earlier intact records are still
returned.

`EventLogWriter::append` checks both `fwrite` and `fflush` before reporting success. M7
does not claim `fsync` or durable-to-disk semantics; the guarantee is stdio flush
correctness for the append path.

`EventLogReader::read_all` distinguishes a missing or unreadable file from a valid empty
log: open/read failures return `LogError::OpenFailed`, while an existing empty file reads
cleanly as zero records.

`apps/qsl-loginspect` is a small CLI that prints a human-readable summary of a log file
(record count, sequence range, command/event counts, and status).

## Deterministic replay and recovery (M8)

The log is a stream of `Command` records (`include/qsl/replay/command.hpp`). The recordable
command set is `RegisterSymbol`, `NewLimit`, `NewMarket`, `Cancel`, `Modify` — a
`std::variant` serialized with a 1-byte tag plus fixed-width fields (`RegisterSymbol`
carries a variable-length name). Including `RegisterSymbol` makes a log **self-contained**:
registering the same names in the same order reproduces identical `SymbolId`s, so the
commands' numeric symbol references line up on replay.

`replay(engine, records)` (`replay/recovery.hpp`) rebuilds state by decoding each command
record and applying it to a fresh engine in order; non-command records and undecodable
payloads are skipped. Because the engine is deterministic and wall-clock independent,
applying the same command stream reproduces the same state and the same emitted events.

### Replay invariant

```text
fresh engine + replay(log) == original engine final state
```

### Comparison dimensions

`EngineSnapshot` (compared with a defaulted `==`) covers, per symbol ordered by `SymbolId`:

- best bid / best ask,
- resting order count,
- **aggregate quantity at each price level** (`bids`/`asks` as `LevelView{price, quantity}`,
  best price first),

plus the engine's **last sequence number**. The **emitted trade/event sequence** is compared
separately by replaying and checking the event stream equals the original. The
replay-equivalence test drives a deterministic synthetic flow (fixed RNG seed,
`generate_flow`) of limit/market/cancel/modify commands across several symbols, then asserts
both the final snapshot and the full event sequence match.

### Recovery CLI

```text
qsl-replay generate <file> [seed]   # write a deterministic synthetic-flow command log
qsl-replay <file>                   # rebuild engine state from the log and print it
```

Generation checks every `EventLogWriter::append` result and exits nonzero on write or
flush failure instead of reporting a complete generated log. Replay exits nonzero when the
requested log cannot be opened or read; `records: 0` is only reported for a real log that
was opened and decoded cleanly as empty. M8 tests include a file-level
`EventLogWriter -> EventLogReader -> replay` round trip through the M7 byte framing.

### Limitations

- Replay reconstructs from the recorded **command** stream; emitted events are recomputed,
  not read back from the log (the log could also store events, but the engine is the source
  of truth for replay equivalence).
- The reader loads the whole log into memory before replaying (adequate for the simulator).
- Commands are trusted once their record checksum validates (M7); the command codec does not
  re-validate enum domains — wire-level enum validation lives at the protocol boundary (M2)
  and risk checks at the gateway (M5).

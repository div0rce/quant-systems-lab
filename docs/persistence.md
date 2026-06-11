# Persistence and Durability (M45)

This document defines the persistence semantics and failure model of the append-only event
log, and analyzes the gap between the lab's logging discipline and an exchange-grade
write-ahead log (WAL). It accompanies the M45 prototype: explicit durability modes,
torn-tail recovery classification, automated repair, and a SIGKILL crash-validation harness.

**This simulator is not production durable and does not claim to be.** The value here is the
failure-model reasoning, the explicit recovery contract, and the evidence that the contract
holds under the failures we can actually inject.

## Where an appended byte lives

An `EventLogWriter::append` moves a record through up to three layers of buffering. Each
durability mode stops at a different layer, and each layer dies with a different failure:

| Layer                   | Reached by                            | Lost on               |
|-------------------------|---------------------------------------|-----------------------|
| user-space stdio buffer | `BufferedOnly`                        | process crash         |
| kernel page cache       | `FlushOnAppend` (`fflush`)            | OS crash / power loss |
| stable storage          | `FsyncOnAppend` (`fflush` + `fsync`)  | media failure         |

- `FlushOnAppend` is the default and preserves the pre-M45 behavior (`fwrite` + `fflush`
  per append).
- `FsyncOnAppend` additionally asks the kernel to reach stable storage on every append. On
  macOS, `fsync` only pushes data to the drive, not necessarily through the drive's volatile
  write cache; the writer therefore uses `F_FULLFSYNC` where the filesystem supports it and
  falls back to `fsync`. On Linux, what `fsync` guarantees past the filesystem journal
  depends on the filesystem, mount options, and whether the hardware honors flush commands.
- `EventLogWriter::sync()` is an explicit group-commit point: it flushes and fsyncs
  regardless of mode, so a caller can batch appends in a weaker mode and pay the
  stable-storage cost once per batch.
- In `FsyncOnAppend` mode the writer also fsyncs the parent directory when it opens the
  log: a new file's bytes can be durable while its *name* is not, in which case the file
  itself can vanish after a power loss.

## Failure model and recovery contract

The recovery scan (`recover_log` / `recover_log_file`) decodes the longest valid record
prefix, then classifies the tail:

| Tail state  | Meaning                                                            | Automated repair |
|-------------|--------------------------------------------------------------------|------------------|
| `CleanTail` | the log ends exactly on a record boundary                          | no-op            |
| `TornTail`  | damage confined to the final record (crash mid-append)             | truncate to last valid boundary |
| `Corrupt`   | damage **not** confined to the final record                        | refused          |

Classification rules:

- A `Truncated` scan error can only occur at the end of the buffer, so it is always a torn
  tail.
- A `BadChecksum` record is a torn tail only when its frame ends exactly at the end of the
  file — consistent with an interrupted final append (for example, out-of-order page
  writeback inside the last record). A checksum failure *followed by more bytes* means valid
  acknowledged records may sit beyond the damage, so it is `Corrupt`.
- A `PayloadTooLarge` header is never trusted: its declared size is garbage, so the extent
  of the damage cannot be proven confined to one append. It is `Corrupt`.

Repair (`repair_log_file`, `qsl-replay recover <file> --repair`) truncates a torn tail back
to the last valid record boundary and fsyncs, so the truncation itself cannot be undone by a
later crash. Repair refuses `Corrupt` logs and leaves them untouched: truncating mid-file
damage would silently discard every record after it, and that data-loss decision belongs to
a human, not automation.

The resulting contract, per mode:

- **`FsyncOnAppend`:** an append acknowledged as successful is in the valid prefix and is
  never removed by tail repair — unless the storage stack lied about flushing, which the
  simulator cannot detect or fix.
- **`FlushOnAppend`:** an acknowledged append survives any process crash, and tail repair
  preserves it; an OS crash or power loss may lose it from the page cache.
- **`BufferedOnly`:** an acknowledged append can be lost outright on process crash (it may
  still be in the user-space stdio buffer). This mode exists to make the cost of skipping
  `fflush` observable, not for real logging.

Residual ambiguity, stated honestly: a checksum failure on the *final* record is
indistinguishable from genuine bit rot of that record. Repairing it discards what might
have been an acknowledged record. Under `FsyncOnAppend` that combination requires the
medium to have lied; under weaker modes it is accepted as part of the mode's contract.

## WAL analysis: the lab log vs. an exchange-grade WAL

The event log already has WAL-like bones: append-only framing, per-record checksums,
deterministic replay from a self-contained command stream, and (with M45) torn-tail
recovery. The honest differences:

| Discipline                  | Exchange-grade WAL                              | This lab today                          |
|-----------------------------|--------------------------------------------------|-----------------------------------------|
| Write ordering              | log **before** applying to the book              | log is written downstream of the engine (log-behind observer) |
| Acknowledgment              | client ack only after the log entry is durable   | gateway acks immediately; durability is decoupled |
| Commit batching             | group commit amortizes fsync across requests     | per-append mode or manual `sync()`      |
| Segmentation                | fixed-size segments, rotation, archival          | one growing file                        |
| Compaction                  | snapshots bound replay length                    | full replay from the start of the log   |
| Recovery objective          | bounded restart time (snapshot + log tail)       | unbounded (proportional to log length); M46 measures this |

The deepest gap is the first two rows. Because the gateway acknowledges orders before
anything is persisted, a crash can lose commands the client believes were accepted — the
simulator's recovery guarantee is "replay reproduces whatever reached the log," not "replay
reproduces everything that was acknowledged." Closing that gap would mean routing the
pipeline through a log-then-apply-then-ack sequence and was deliberately left out of M45:
it rearchitects the threaded pipeline for a durability property the simulator does not
claim. M45 scopes durability to the log layer itself and makes the modes, costs, and
recovery semantics explicit there.

Segmentation, rotation, and snapshot-bounded recovery are likewise documented as gaps, not
built; M46 (recovery benchmarking) measures the cost of the current full-replay design
before any of that is added.

## Validation

Two layers of automated evidence, each labeled with what it does and does not prove:

1. **Deterministic unit tests** (`tests/unit/test_event_log.cpp`, `[recovery]` tag): a log
   cut at *every byte offset* recovers the exact valid prefix with the correct
   classification; final-record checksum damage classifies as torn; mid-file damage and
   untrusted headers classify as corrupt and are refused by repair; repaired logs read
   clean and accept appends; every durability mode round-trips.
2. **Process-kill harness** (`make crash-recovery`,
   `scripts/crash_recovery_validation.sh`, artifact
   `results/crash_recovery_validation.txt`): SIGKILLs a live writer mid-stream per mode and
   checks that flush/fsync modes preserve every acknowledged record (with at most one
   unacknowledged in-flight record appearing), that torn tails repair to clean appendable
   logs, and that buffered mode demonstrably loses its unflushed tail.

What this validation **cannot** show: SIGKILL leaves the kernel page cache intact, so the
harness validates crash-mid-append recovery and process-death retention only. Power-loss
and OS-crash durability would require fault injection below the filesystem (or pulling
plugs), neither of which this repo performs — so the `FsyncOnAppend` stable-storage
guarantee is exercised but not falsified here, and no such durability is claimed.

## Limits

- No production-durability claim, in any mode.
- No power-loss or OS-crash testing; `fsync`/`F_FULLFSYNC` behavior below the filesystem is
  taken on faith and varies by OS, filesystem, and hardware.
- Acknowledgment is not coupled to durability anywhere in the gateway/pipeline path.
- One log file; no segments, rotation, snapshots, or bounded recovery time.
- Recovery loads the whole log into memory (adequate for the simulator).
- `Corrupt` logs are intentionally not auto-repaired.

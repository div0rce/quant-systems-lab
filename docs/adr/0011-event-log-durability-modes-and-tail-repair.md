# ADR 0011: Event-Log Durability Modes and Conservative Tail Repair

## Status

Accepted

## Context

Through M44 the event log guaranteed only stdio flush (`fwrite` + `fflush`) per append: an
acknowledged append survived a process crash but not an OS crash or power loss, and the
reader returned the longest valid prefix without distinguishing a benign crash-torn tail
from mid-file corruption. M45 investigates a stronger durability strategy without claiming
production durability.

Two design questions had real alternatives:

1. Where should durability be enforced — per append, per batch, or never — and who decides?
2. What damage may automation repair without a human?

## Decision

1. **Durability is an explicit caller-chosen mode, not a hidden default.**
   `EventLogWriter` takes `DurabilityMode` (`BufferedOnly`, `FlushOnAppend`,
   `FsyncOnAppend`); the default `FlushOnAppend` preserves pre-M45 behavior so existing
   callers are unchanged. `sync()` provides an explicit group-commit point. On macOS,
   fsync uses `F_FULLFSYNC` with `fsync` fallback; `FsyncOnAppend` also fsyncs the parent
   directory at open so a new log's directory entry is durable, not just its bytes.
2. **Automation repairs only damage it can prove is confined to the final append.**
   Recovery classifies the tail as `CleanTail`, `TornTail`, or `Corrupt`. `Truncated` is
   always torn (it can only occur at end of buffer). `BadChecksum` is torn only when the
   failing frame ends exactly at end of file. `PayloadTooLarge` headers are never trusted.
   `repair_log_file` truncates torn tails to the last valid record boundary (and fsyncs
   the truncation); it refuses `Corrupt` logs, because truncating mid-file damage would
   silently discard acknowledged records beyond it — a human decision.
3. **The claim is scoped to the log layer.** The gateway/pipeline still acknowledges
   before persisting (log-behind, not write-ahead). Re-architecting acks-after-durability
   was deliberately rejected for M45; the gap is documented in `docs/persistence.md`
   instead of half-built.

## Consequences

- Tests can assert the recovery contract byte-for-byte (truncation sweep at every offset),
  and `make crash-recovery` validates it under real SIGKILL, including demonstrated
  acknowledged-data loss in `BufferedOnly` mode.
- The repo can describe durability honestly per mode: process-crash-safe (`FlushOnAppend`),
  asked-to-be-stable (`FsyncOnAppend`), or deliberately weak (`BufferedOnly`).
- Power-loss/OS-crash durability remains unvalidated and unclaimed: SIGKILL cannot falsify
  it, and no sub-filesystem fault injection exists here.
- A final-record checksum failure is indistinguishable from bit rot of that record; repair
  accepts that ambiguity and the docs state it.
- Segmentation, snapshots, and bounded recovery time remain out of scope; M46 measures the
  full-replay recovery cost before any such design is considered.

# ADR 0002: Independent OCaml differential oracle

## Status

Accepted

## Context

The C++ engine's own unit tests share code and assumptions with the engine, so a bug in a
shared assumption can pass unnoticed. We wanted a cross-check that does not share the C++
implementation, and a Jane Street-relevant language signal, without claiming formal verification.

## Decision

Implement a second matching engine in OCaml (`ocaml/lib/replay_engine.ml`) that replays the
exported **command stream only** (ignoring the C++ event/snapshot output) and computes its own
final snapshot. The differential test asserts the two snapshots render to byte-identical lines
(`snapshot_to_lines`). The OCaml side uses immutable data (`Map`/FIFO lists), a different memory
model from the C++ intrusive book.

## Consequences

- A transcription/logic/data-structure bug on one side produces a different snapshot and is caught.
- The oracle is genuinely independent in language and data model; an audit
  (`docs/ocaml_verifier.md`) records where it deliberately mirrors C++ semantics and what that
  cannot catch (shared-spec errors, reject *reasons*, serialization format).
- It is **not** formal verification and proves agreement only over tested inputs.

## Alternatives considered

- **C++-only property tests** — rejected: they share the engine's assumptions.
- **A second C++ engine** — rejected: a same-language reimplementation shares more failure modes
  and gives no language-diversity signal.
- **Consuming the C++ event log** instead of replaying commands — rejected: that would trust the
  system under test rather than independently deriving state.

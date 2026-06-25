# ADR 0004: Deterministic shrinker and minimal failing fixtures

## Status

Accepted

## Context

A randomized property failure on a 100+ command stream is hard to debug. We wanted failures to
reduce to small, reviewable counterexamples, deterministically, without a property-testing
dependency.

## Decision

Implement a greedy, deterministic delta-debug shrinker (`replay::shrink`) that preserves a
pluggable failure predicate and iterates to a fixed point: remove contiguous chunks, remove
single commands, simplify fields (quantities and limit/modify prices toward 1), and renumber
(drop unreferenced symbol registrations; compact symbol/order ids). Every transform is
predicate-guarded, so a step that breaks the failure is discarded. The result is 1-minimal under
single-command removal. Minimal counterexamples are exported as fixtures and notable ones are
kept in a regression archive (`regressions/`); on a CI divergence a failure artifact bundle
(original/computed/expected/diff) is uploaded.

## Consequences

- Failures become small fixtures (e.g. a 123-command flow reduces to 3 commands).
- Determinism makes counterexamples reproducible and golden-checkable.
- It is greedy, not globally minimal; renumbering bails on duplicate registration names because
  registration is idempotent and the positional id model would otherwise misrepresent the stream.

## Alternatives considered

- **A property-testing library (e.g. QuickCheck-style dependency)**, rejected: the project keeps
  dependencies minimal, and a bespoke shrinker over the command ADT is small and transparent.
- **Random/ddmin without field simplification**, rejected: leaves large prices/ids and extra
  registrations, producing less readable counterexamples.

# ADR 0005: Dynamic Concurrency Validation Is Evidence, Not Proof

## Status

Accepted

## Context

M27 added ThreadSanitizer coverage for the concurrent pipeline and runs the concurrent test suites
under TSan where supported. TSan is valuable because it can detect data races that occur during
executed schedules.

Dynamic tools do not explore every possible thread interleaving. Stress tests, capacity sweeps,
and sanitizer runs improve confidence, but they are empirical evidence rather than a proof of
concurrency correctness.

## Decision

The repository will describe TSan as dynamic data-race evidence over executed schedules, not as a
correctness proof. Concurrency claims must distinguish:

- static happens-before reasoning;
- dynamic TSan evidence;
- stress/backpressure tests;
- remaining untested schedules and assumptions.

## Consequences

Future concurrency work belongs in M33 and may add randomized scheduling perturbation, longer
Linux stress runs, expanded capacity coverage, and additional happens-before documentation. The
repo must not state that TSan proves all interleavings correct.

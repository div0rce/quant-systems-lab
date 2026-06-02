# ADR 0007: Constrained Perf Artifacts Are Partial Evidence

## Status

Accepted

## Context

M29 added Linux `perf` scripts and metadata-rich artifacts. The available Docker Desktop Linux
environment can run the scripts, but it does not expose usable hardware PMU counters or sampling
permissions. The generated artifacts therefore validate the workflow and metadata path, not real
hardware PMU behavior.

Treating constrained Docker output as full PMU evidence would make the profiling artifacts
misleading.

## Decision

The repository accepts constrained-environment perf artifacts only as partial evidence. They must
be labeled as constrained validation and must not be described as full hardware PMU evidence.

Full PMU evidence requires a bare-metal Linux host or a Linux VM/server with hardware `perf_event`
access, no permission failures, no unsupported counters, clean git metadata, and numeric values for
the required hardware counters. Issue #90 tracks that follow-up.

## Consequences

M29 can land the workflow and constrained validation without overclaiming. Full hardware-counter
evidence remains an explicit evidence debt rather than an implicit deficiency or fabricated result.

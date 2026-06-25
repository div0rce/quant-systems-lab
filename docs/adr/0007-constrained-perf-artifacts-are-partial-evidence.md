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

## Update (2026-06, v0.2.0): bare-metal partial PMU evidence

The development host moved from Docker Desktop to a **bare-metal** Apple MacBook Air (M2, aarch64)
running Fedora Asahi Remix (`systemd-detect-virt: none`). `perf stat` now reads **real hardware
counters** off the Apple Avalanche/Blizzard PMUs (`cycles`, `instructions`, `branches`,
`branch-misses`), so the committed artifact is no longer "no PMU at all."

`scripts/perf_stat.sh` now classifies three ways instead of two:

- `hardware PMU evidence`, every requested counter, including cache events, captured (full).
- `partial hardware PMU evidence`, at least one real hardware counter captured, but the requested
  set is incomplete. **This is the current Apple Silicon state**: `cache-references`/`cache-misses`
  are `<not supported>` because the Asahi PMU driver does not expose them.
- `constrained-environment validation (no hardware PMU access)`, no hardware counter produced a
  value (the prior Docker case).

The original decision stands: a partial artifact is partial evidence and must not be called *full*.
What changed is that the honest label is now `partial hardware PMU evidence` (real counters present),
not `constrained validation`. Issue #90's residual is narrowed to the cache-counter set, which needs
a PMU microarchitecture that exposes those events (x86_64, or an ARM server core), being bare metal
is necessary but not sufficient.

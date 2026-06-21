# Linux Perf Analysis

M29 adds Linux `perf` profiling scripts and documents how to interpret their output. This is
performance-investigation evidence, not an optimization pass and not a production latency claim.

## Current Status

The Linux `perf` workflow provides Linux-only tooling, metadata-rich artifacts, dirty-tree
handling, PMU preflight/validation, a three-way evidence classification (full / partial / none),
CI validation, and a reproducible command path.

The committed artifacts are now generated on a **bare-metal Linux host** — an Apple MacBook Air
(M2, aarch64) running Fedora Asahi Remix, directly on the hardware (`systemd-detect-virt` reports
`none`, no `hypervisor` CPU flag). On this heterogeneous SoC `perf` opens each event against both
PMU instances — the Apple Avalanche (P-core) and Blizzard (E-core) PMUs — but the single-threaded
benchmark is scheduled on the performance cores, so **the Avalanche counters carry the real
counts**: `cycles`, `instructions`, `branches`, and `branch-misses` are live there. The
corresponding `apple_blizzard_pmu/...` rows read `<not counted>` in `results/perf_stat_linux.txt`
because the workload never ran on the E-cores — that is expected scheduling behavior, not a missing
counter. The artifact is therefore classified **partial hardware PMU evidence**, not
constrained-environment validation: the counters that are present are real, not emulated.

The residual gap is specific and is what issue #90 now tracks: the Apple Silicon PMU, as exposed
by the current Asahi kernel driver, does **not** implement the generic `cache-references` /
`cache-misses` events (it whitelists only `cycles`/`instructions`/branch events; the
`/sys/bus/event_source/devices/apple_avalanche_pmu/events/` directory lists only `cycles` and
`instructions`). So a *full* counter set — including cache events — is unavailable here. Closing
#90 needs a PMU **microarchitecture** that exposes cache counters to Linux (e.g. an x86_64
Intel/AMD host, or an ARM server core such as Graviton/Ampere) — not "more bare metal."

## Commands

Build the benchmark preset and run `perf stat`:

```bash
make perf-stat
```

This runs `scripts/perf_stat.sh`, which records metadata and the raw `perf stat` output in
`results/perf_stat_linux.txt`. The required event set is:

```text
cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,page-faults
```

Build the benchmark preset and run `perf record` / `perf report`:

```bash
make perf-record
```

This runs `scripts/perf_record.sh`, which writes a text report to
`results/perf_report_linux.txt`. By default it uses software `cpu-clock` sampling at 2000 Hz. That
default is intentional: many CI, VM, and container environments do not expose hardware PMU events
to unprivileged processes, and the benchmark harness is short enough that a lower frequency can
miss the minimum sample count needed for meaningful hot-symbol ordering.

Render a flamegraph (issue #32):

```bash
make flamegraph
```

This runs `scripts/flamegraph.sh`, which records call-graph samples
(`perf record --call-graph dwarf -F 4000 -g -e cpu-clock`), folds them, and renders an SVG to
`results/flamegraph.svg` plus a text companion `results/flamegraph.txt` (provenance, classification,
and the top folded stacks). DWARF call graphs are used so stacks unwind correctly even though the
`bench` (Release) preset omits frame pointers — the application symbols (`OrderBook::add_limit`,
`MatchingEngine::new_limit`, the replay path, …) resolve from the symbol table without changing the
optimization level under measurement.

The folding and SVG rendering live in `scripts/flamegraph.py`, a dependency-free Python script
(standard library only) that reimplements the `stackcollapse` + flamegraph data model rather than
vendoring Brendan Gregg's Perl toolkit, so the artifact is reproducible from this repository alone.
The renderer is deterministic — frames are sorted by name and colors are a pure function of the
frame name (no RNG, no timestamps in the drawn body) — and is unit-tested in
`tests/shell/test_flamegraph.sh` (registered with CTest, runs under `make check`). Frame width is
proportional to on-CPU samples; this is a software cpu-clock sampling profile for **hot-symbol
investigation**, not a latency or throughput measurement. Set `QSL_FLAMEGRAPH_EVENT=cycles` to
sample the hardware PMU cycles event instead, where the host exposes it.

## Required Environment

Both scripts are Linux-only and fail before running on non-Linux hosts. `perf stat` also fails
unless the kernel and permissions expose the requested PMU events. On many systems this depends on:

- `kernel.perf_event_paranoid`;
- whether the process has enough privilege for hardware counters;
- whether the machine is bare metal, a VM, a container, or a hosted CI runner;
- CPU/kernel support for the requested events. Being bare metal is necessary but not sufficient:
  this Apple Silicon host is bare metal yet its PMU driver still does not expose cache events.

Full hardware-counter evidence requires a host whose PMU exposes the **whole** requested event set
(including `cache-references`/`cache-misses`) through `perf_event` — e.g. an x86_64 Intel/AMD box or
an ARM server core — not merely any bare-metal Linux machine. Before trusting a `perf stat` artifact
as full evidence,
verify:

```bash
uname -a
perf --version
cat /proc/sys/kernel/perf_event_paranoid
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses -- true
```

The script classifies the artifact three ways via its `Artifact:` and `Hardware counters
supported:` fields:

- **`hardware PMU evidence`** (`Hardware counters supported: yes`) — every requested counter,
  including cache events, was captured. This is *full* evidence.
- **`partial hardware PMU evidence`** (`Hardware counters supported: partial`) — at least one real
  hardware counter was captured but the requested set is incomplete. This is the **current state on
  the Apple Silicon host**: real `cycles`/`instructions`/`branches`/`branch-misses`, with
  `cache-references`/`cache-misses` reported `<not supported>`.
- **`constrained-environment validation (no hardware PMU access)`** (`Hardware counters supported:
  no`) — no hardware counter produced a value at all (a VM/container with no PMU, or a
  permission-denied host). This was the state of the earlier Docker Desktop artifacts.

A *full* artifact is acceptable only when it reports `Artifact: hardware PMU evidence`,
`Unsupported counters detected: no`, `Hardware counters supported: yes`, and `Dirty inputs: no`.
On Apple Silicon that bar cannot be met for the cache events, so the honest current label is
**partial**, not full — and that is recorded in the artifact rather than papered over.

If the host reports counters as unsupported or permission-denied, do not substitute other numbers.
The scripts only write a partial/constrained artifact with `QSL_PERF_ALLOW_PARTIAL=1`, and the
label states exactly which counters were and were not collected.

Partial mode does not hide benchmark failures. Both perf scripts first run the benchmark harness
outside `perf`; if `qsl-bench` exits non-zero, the script exits non-zero even when
`QSL_PERF_ALLOW_PARTIAL=1`. The override only permits constrained perf evidence such as unsupported
counters, permission-limited sampling, or a sample report that is explicitly marked insufficient.

## Artifacts

- `results/perf_stat_linux.txt` records benchmark output plus raw `perf stat` counters. Check its
  `Artifact:` field before treating it as full hardware-counter evidence.
- `results/perf_report_linux.txt` records benchmark output, `perf record` stderr, and
  `perf report --stdio` output. It is useful as a hot-symbol profile only when `No samples: no`,
  `Insufficient samples: no`, and `Sample count` is at least `Minimum samples for hot profile`.
- `results/flamegraph.svg` is the rendered flamegraph from `make flamegraph`; `results/flamegraph.txt`
  is its provenance/classification companion (and lists the top folded stacks). Treat frame widths as
  a hot-symbol guide only when the `.txt` reports a `flamegraph (...)` `Artifact:` and a `Sample
  count` at least `Minimum samples for hot profile`; a `constrained-environment validation` label
  means sampling did not capture enough stacks to trust.
- `build/perf/qsl-bench.perf.data` and `build/perf/qsl-bench.flame.data` are generated by
  `make perf-record` / `make flamegraph` and are intentionally not committed; they are host-specific
  binary profiler data.

Each artifact includes hardware, kernel, compiler, perf version, build type, dataset, command,
event set, and source-digest provenance. The `Source digest` is the authoritative source identity;
`Git commit (informational)` is context only and can change after squash/rebase without making the
artifact stale.

The `Dirty inputs` field checks the declared source inputs and excludes the generated perf output
itself, so a two-artifact run can remain honest while still detecting real source or script changes.

The committed artifacts are now bare-metal Apple Silicon Linux runs labeled **partial hardware PMU
evidence**: real `cycles`/`instructions`/`branches`/`branch-misses` plus `<not supported>` cache
events. Do not describe them as *full* hardware-counter evidence (the cache events are missing), and
do not relabel them back to "constrained validation" either — the counters that are present are
genuine bare-metal hardware counters.

## What To Look For

The default `make perf-stat` and `make perf-record` targets profile the default `qsl-bench`
synthetic suite: order-book operations, protocol codec work, gateway/session handling, matching
flow application, and replay. They do not profile the separate differential harness
(`qsl-bench diff` / `make bench-diff`) or the M28 allocator experiment (`qsl-bench pool` /
`make bench-allocator`).

A useful profile should separate where time is spent instead of treating the aggregate number as
one result:

- order-book scenarios should make tree/list operations and matching paths visible;
- protocol scenarios should make encode/decode helpers visible;
- replay scenarios should show command dispatch, gateway checks, and engine apply paths.

Do not use the default M29 perf artifacts to draw conclusions about differential-replay or
allocator hot paths. Those workloads need their own explicit perf run before they can support
optimization work.

M29 deliberately does not optimize anything based on these reports. Any future optimization should
first point to a concrete hot path in `perf report`, then add a separate benchmark or regression
check that proves the change helped on the stated machine.

## Limits

These profiles are machine-specific. They do not prove production latency, network behavior, or
kernel/socket-path performance. The socket / kernel-path profiling is a separate study
(`docs/socket_profiling.md`, `make profile-io` / `make socket-load` / `make socket-stress`); this
perf workflow only profiles the default CPU-side benchmark harness.

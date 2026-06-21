# Socket / kernel-path profiling

This documents how the gateway and market-data feed are profiled at the socket / kernel
boundary, what the committed artifacts mean, and — just as important — what they do **not**
prove. It complements [`socket_gateway.md`](socket_gateway.md) (the design),
[`socket_hardening.md`](socket_hardening.md) (the defensive posture), and
[`linux_performance.md`](linux_performance.md) / [`perf_analysis.md`](perf_analysis.md) (the
CPU-side perf workflow). Late-stage kernel-bypass research is tracked separately in
[`dpdk_research.md`](dpdk_research.md); it is not part of these socket artifacts.

All numbers here are **measured by committed scripts** on a developer machine over **loopback**.
They are profiling evidence for investigation, not a latency, throughput, or capacity claim. See
[Limitations](#limitations).

## What is profiled

Three distinct paths, three scripts:

| Path | Transport | Script | `make` target | OS |
|------|-----------|--------|---------------|----|
| TCP order gateway | TCP, sequential short-lived clients | `scripts/profile_gateway_io.sh` | `make profile-io` | Linux only |
| Market-data feed | UDP unicast | `scripts/socket_stress.sh` | `make socket-stress` | Linux + macOS |
| TCP connection-scaling load | TCP, threaded vs epoll | `scripts/socket_load.sh` | `make socket-load` | Linux only |

The gateway profile needs `strace` and Linux procfs (`/proc/<pid>/{stat,status}`), so that script
**fails clearly on non-Linux** (exit 2), exactly like the M29 `perf` scripts. The UDP
experiment uses only portable BSD-socket behaviour, so it runs on both Linux and macOS and
records which OS produced the artifact.

## Gateway syscall / kernel-socket profile (`make profile-io`)

Artifact: [`results/socket_profile_loopback.txt`](../results/socket_profile_loopback.txt).

The script drives a fixed load — `CONNECTIONS` sequential `qsl-client` round trips, each a
`connect` → `NewOrder` + `Heartbeat` → read replies → `close` — against `qsl-gateway`, in two
passes over the same workload:

1. **rusage pass** — reads the gateway's `/proc/<pid>/{stat,status}`: user (engine-side) vs
   system (kernel/socket) CPU time, voluntary/involuntary context switches, minor/major page
   faults, and peak RSS (`VmHWM`). This is the pass that **distinguishes user-space matching cost
   from kernel/socket overhead**. No tracer is attached, so perturbation is minimal.
2. **`strace -f -c` pass** — per-syscall call counts and time-in-kernel, i.e. *which* syscalls
   the socket path spends its time in (expected: `accept`, `read`/`recvfrom`, `write`/`sendto`,
   `close`, plus connection setup). `strace` multiplies syscall cost dramatically, so this pass
   is read for the **syscall mix**, never for wall-clock seconds.

Pass 1 starts the gateway **directly** (the script owns its PID) and reads its procfs rusage;
pass 2 runs the gateway **under** `strace -f -c` so the gateway is strace's *descendant* — that
relationship is what lets tracing work under the common Yama `ptrace_scope=1` default, where a
tracer may only trace its own descendants (attaching to a sibling would need `CAP_SYS_PTRACE`).
Each pass waits for the gateway to accept a loopback connection, drives the load, then stops the
gateway; the two passes use adjacent ports to avoid `TIME_WAIT` reuse stalls.

### Reading it

- **User vs system CPU (Pass 1)** is the headline: it shows how much of the gateway's work is
  in-process matching/serialization versus kernel time on the socket path. On loopback the
  kernel/socket share is meaningful precisely because the matching work per order is tiny.
- **Context switches / page faults (Pass 1)** characterize scheduling and memory behaviour under
  the connect-per-request pattern (each short-lived client connection wakes the gateway).
- **Syscall counts (Pass 2)** confirm the path is dominated by the expected stream-socket calls
  and that there is no surprising syscall (e.g. unexpected `fcntl`/`poll` churn).

On a representative loopback run (500 round trips, bare-metal Apple M2 Fedora Asahi Linux), the
gateway's *measurable* CPU was effectively all in the kernel/socket path — user-space matching fell
below the clock-tick (10 ms) granularity, with roughly one voluntary context switch per connection —
and the syscall mix was dominated by the per-request `accept` / `read` / `sendto` / `close`
(alongside one-time process and socket setup such as `execve` / `socket` / `bind` / `listen`).
The honest takeaway: for this trivial-per-order loopback workload the socket servicing dominates,
not the matching. The
committed [`results/socket_profile_loopback.txt`](../results/socket_profile_loopback.txt) records
the actual run.

## UDP socket-buffer / burst-loss experiment (`make socket-stress`)

Artifact: [`results/socket_stress_summary.txt`](../results/socket_stress_summary.txt).

`qsl-mdfeed publish` bursts every market-data datagram of a deterministic synthetic flow
back-to-back with no pacing; `qsl-mdfeed subscribe` drains them and reports how many it received.
The authoritative loss metric is **`published − received`**, which also counts datagrams dropped
at the *end* of the burst; the `SequenceTracker` count is reported separately as **interior**
gaps only. The single independent variable is the subscriber's requested `SO_RCVBUF`: a small
request, the OS default, and a large request. The kernel may round up (Linux roughly doubles) or
clamp the request to a system maximum, so the **effective** granted size is read back with
`getsockopt` and reported alongside the request. Each setting is run over several trials.

### Reading it

A too-small receive buffer overflows during the burst and the kernel **silently drops**
datagrams. Read `published − received` (the `lost/trial` column) as the true loss: a datagram
dropped at the very end of the burst leaves no later sequence number to reveal it, so the
interior `seq-gaps` count can read 0 even when datagrams were lost — which is exactly why the
artifact reports loss as `published − received` and treats the sequence-gap count as a secondary,
interior-only signal. The OS default and larger buffers absorb the same burst with little or no
loss. Because UDP loss is timing/OS/load dependent, per-trial counts vary between trials and runs
— a small buffer does **not** lose on every trial — so the mechanism (`SO_RCVBUF` bounds
in-kernel queueing) is the point, not any specific number. A representative loopback run on the
development machine showed loss only at the smallest buffer (from a handful to ~a thousand
datagrams across trials) and none at the default and large buffers; the committed artifact
records the actual run.

This directly motivates the receive-buffer tuning knob documented in
[`socket_hardening.md`](socket_hardening.md#receive-buffer-sizing-so_rcvbuf).

## Reproduce

```bash
# Linux only — syscall/rusage profile of the gateway path:
make profile-io
#   tunables: QSL_PROFILE_CONNECTIONS (default 500), QSL_PROFILE_PORT

# Linux or macOS — UDP buffer/loss experiment:
make socket-stress
#   tunables: QSL_STRESS_ORDERS, QSL_STRESS_TRIALS, QSL_STRESS_SMALL_BUF, QSL_STRESS_LARGE_BUF

# Linux only — multi-client threaded-vs-epoll connection-scaling load:
make socket-load
#   tunables: QSL_LOAD_COUNTS, QSL_LOAD_TRIALS, QSL_LOAD_PORT, QSL_LOAD_ALLOW_PARTIAL
```

The committed gateway artifact is now generated on a **bare-metal** Apple M2 (aarch64) Fedora Asahi
Linux host (which has `strace` and procfs natively); the earlier macOS development host had no
`strace`, so prior versions were produced in containerized Linux. It remains **loopback-only**
evidence — real NIC/driver/routing behaviour is still not exercised — and its metadata records the
OS, kernel, compiler, source digest, and working-tree state it was produced from.

## Limitations

- **Loopback only.** No NIC, device driver, queue discipline, routing, or real-network loss /
  reordering / latency is exercised. Loopback removes exactly the parts that dominate real
  network cost.
- **Transport scope differs by artifact.** The M30 gateway profile covers sequential
  short-lived client round trips; `scripts/socket_load.sh` / `results/socket_load_summary.txt`
  compare threaded and epoll transports under a bounded multi-client loopback load.
- **`strace` perturbs timing.** Use Pass 1 (procfs rusage) for the user/kernel CPU split; use
  Pass 2 only for the syscall *mix*.
- **Synthetic, deterministic flow.** The workload is the repo's seeded synthetic flow, not real
  order traffic.
- Results are **hardware/kernel/OS dependent** and will differ across machines.

## Multi-client connection-scaling load (`make socket-load`)

Artifact: [`results/socket_load_summary.txt`](../results/socket_load_summary.txt).

`scripts/socket_load.sh` (Linux-only) drives **N concurrent** short-lived clients (`qsl-client`:
connect → `NewOrder` + `Heartbeat` → read replies → close) against `qsl-gateway` in **both**
transport modes — the portable threaded TCP server and the epoll event loop (M34) — across a
sweep of client counts, reporting the best (minimum) wall time and an approximate
connections/second per cell.

### Reading it

Per-order matching is sub-microsecond, so the wall time is the **connection-setup / accept /
socket path**, not engine cost. At these small loopback counts connection setup can dominate and
the two modes may stay **close**: the honest read of the artifact is transport comparison under
this constrained load, not a demonstrated general win for either mode. The absolute
conns/s figures are loopback, single-machine, and bounded by client process-spawn cost, so they are
**not** a production-capacity or throughput claim. The script is Linux-only (epoll mode + the high-resolution
timer) and skips cleanly elsewhere; the committed artifact is regenerated with `make socket-load`
on the bare-metal Apple M2 Linux host (loopback), like the gateway profile above. Receiver-side socket-buffer pressure is
covered separately by the UDP experiment ([`make socket-stress`](#udp-socket-buffer--burst-loss-experiment-make-socket-stress)).

## What this does and does not show

It **does** show: where the gateway's time splits between user-space work and the kernel socket
path; the syscall mix of that path; and that an undersized UDP receive buffer causes observable,
sequence-visible datagram loss under burst while adequate buffers do not.

It **does not** show: production latency or throughput, behaviour over a real network, production
throughput under concurrency (the load test shows connection-scaling *shape*, not capacity), or
any kernel-bypass / low-latency-networking result. No such claim is made; see
[`dpdk_research.md`](dpdk_research.md) for the explicit environment gate that would be required
before any DPDK prototype evidence exists.

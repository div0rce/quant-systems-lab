# Socket / kernel-path profiling

This documents how the gateway and market-data feed are profiled at the socket / kernel
boundary, what the committed artifacts mean, and — just as important — what they do **not**
prove. It complements [`socket_gateway.md`](socket_gateway.md) (the design),
[`socket_hardening.md`](socket_hardening.md) (the defensive posture), and
[`linux_performance.md`](linux_performance.md) / [`perf_analysis.md`](perf_analysis.md) (the
CPU-side perf workflow).

All numbers here are **measured by committed scripts** on a developer machine over **loopback**.
They are profiling evidence for investigation, not a latency, throughput, or capacity claim. See
[Limitations](#limitations).

## What is profiled

Two distinct paths, two scripts:

| Path | Transport | Script | `make` target | OS |
|------|-----------|--------|---------------|----|
| TCP order gateway | TCP, one connection at a time | `scripts/profile_gateway_io.sh` | `make profile-io` | Linux only |
| Market-data feed | UDP unicast | `scripts/socket_stress.sh` | `make socket-stress` | Linux + macOS |

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

Each pass starts the gateway **directly** (the script owns its PID), waits for it to accept a
loopback connection, drives the load, then snapshots procfs (pass 1) or lets `strace` summarize
on exit (pass 2) before stopping the gateway. The two passes use adjacent ports to avoid
`TIME_WAIT` reuse stalls.

### Reading it

- **User vs system CPU (Pass 1)** is the headline: it shows how much of the gateway's work is
  in-process matching/serialization versus kernel time on the socket path. On loopback the
  kernel/socket share is meaningful precisely because the matching work per order is tiny.
- **Context switches / page faults (Pass 1)** characterize scheduling and memory behaviour under
  the connect-per-request pattern (each short-lived client connection wakes the gateway).
- **Syscall counts (Pass 2)** confirm the path is dominated by the expected stream-socket calls
  and that there is no surprising syscall (e.g. unexpected `fcntl`/`poll` churn).

On a representative constrained run (300 round trips, containerized Linux), the gateway's
*measurable* CPU was effectively all in the kernel/socket path — user-space matching fell below
the clock-tick (10 ms) granularity, with roughly one voluntary context switch per connection —
and the syscall mix was exactly `accept` / `read` / `sendto` / `close`. The honest takeaway: for
this trivial-per-order loopback workload the socket servicing dominates, not the matching. The
committed [`results/socket_profile_loopback.txt`](../results/socket_profile_loopback.txt) records
the actual run.

## UDP socket-buffer / burst-loss experiment (`make socket-stress`)

Artifact: [`results/socket_stress_summary.txt`](../results/socket_stress_summary.txt).

`qsl-mdfeed publish` bursts every market-data datagram of a deterministic synthetic flow
back-to-back with no pacing; `qsl-mdfeed subscribe` drains them while the `SequenceTracker`
counts gaps (missed sequence numbers). The single independent variable is the subscriber's
requested `SO_RCVBUF`: a small request, the OS default, and a large request. The kernel may
round up (Linux roughly doubles) or clamp the request to a system maximum, so the **effective**
granted size is read back with `getsockopt` and reported alongside the request. Each setting is
run over several trials.

### Reading it

A too-small receive buffer overflows during the burst and the kernel **silently drops**
datagrams, which surface as sequence gaps. The OS default and larger buffers absorb the same
burst with little or no loss. Because UDP loss is timing/OS/load dependent, gap counts vary
between trials and runs — a small buffer does **not** lose on every trial. The mechanism
(`SO_RCVBUF` bounds in-kernel queueing, so an undersized buffer drops under burst) is the point,
not any specific number. A representative loopback run on the development machine showed
intermittent loss only at the smallest buffer (single-digit gaps on some trials) and zero loss
at the default and large buffers; the committed artifact records the actual run.

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
```

The committed gateway artifact was generated in a **containerized Linux** environment (Docker)
because the primary development host is macOS, which has no `strace`. Like the M29 `perf`
artifacts, it is therefore **constrained-environment evidence**, and its metadata records the
OS, kernel, compiler, commit, and working-tree state it was produced from. Regenerate it on a
clean checkout on a bare-metal Linux host for a clean-tree version.

## Limitations

- **Loopback only.** No NIC, device driver, queue discipline, routing, or real-network loss /
  reordering / latency is exercised. Loopback removes exactly the parts that dominate real
  network cost.
- **Single connection at a time.** The gateway (M9) serves one connection at a time by design;
  this profiles that design. Event-driven multi-client serving (`epoll`) and multi-client load
  are intentionally out of M30 scope and tracked as later milestones (M34/M35).
- **`strace` perturbs timing.** Use Pass 1 (procfs rusage) for the user/kernel CPU split; use
  Pass 2 only for the syscall *mix*.
- **Synthetic, deterministic flow.** The workload is the repo's seeded synthetic flow, not real
  order traffic.
- Results are **hardware/kernel/OS dependent** and will differ across machines.

## What this does and does not show

It **does** show: where the gateway's time splits between user-space work and the kernel socket
path; the syscall mix of that path; and that an undersized UDP receive buffer causes observable,
sequence-visible datagram loss under burst while adequate buffers do not.

It **does not** show: production latency or throughput, behaviour over a real network, behaviour
under concurrency, or any kernel-bypass / low-latency-networking result. No such claim is made.

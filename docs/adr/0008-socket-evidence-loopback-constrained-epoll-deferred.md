# ADR 0008: Socket Evidence Is Loopback-Constrained and epoll Was Deferred

## Status

Accepted

## Context

M30 profiles and hardens the Linux socket path of the TCP order gateway and the UDP market-data
feed. Two constraints shape what can honestly be claimed:

1. The gateway syscall / rusage profile needs Linux tools (`strace`, procfs) and was generated
   in containerized Linux (Docker) because the primary development host is macOS. Every socket
   experiment runs over loopback (`127.0.0.1`).
2. An `epoll`-based event-driven gateway was a natural next step, but `epoll` is a Linux-specific
   API that could not be compiled or tested on the macOS development host during M30.

Treating loopback profiling as real-network evidence, or committing untested platform-specific
code, would be misleading and would violate the project's measured-evidence and no-untested-C++
bars.

## Decision

- Socket profiling artifacts are loopback-only, constrained-environment evidence — the same
  policy ADR 0007 applies to `perf`. They are labeled as such, carry OS/kernel/compiler/commit/
  dirty-tree metadata, and never claim NIC/driver/real-network behavior or production capacity.
- M30 profiles and hardens the **existing** one-connection-at-a-time gateway rather than rewriting
  it. The `epoll` multi-client architecture was deferred to **M34**, and multi-client
  socket-pressure testing to **M35**, so no untested Linux-only code was committed in M30.
  ADR 0010 records the later tested M34 epoll prototype. `io_uring` is discussed only.
- The UDP receive-buffer (`SO_RCVBUF`) knob is added and justified by a **measured** burst/loss
  experiment, not by assertion. Loss is detected (sequence gaps), not recovered; there is no
  retransmit/gap-fill channel.

## Consequences

M30 lands a real, measured socket profiling/hardening workflow and a justified hardening knob
without overclaiming. Event-driven serving and real-network/hardware evidence remain explicit
separately testable milestones and acknowledged evidence debts, rather than implicit gaps or
overclaimed artifacts.

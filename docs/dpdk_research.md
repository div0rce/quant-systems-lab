# DPDK Research Notes

M48 is a late-stage networking research milestone. It compares the project's existing kernel
socket evidence with the requirements for a DPDK user-space packet path, without pretending that a
kernel-bypass path has been measured on unsupported hardware.

This document uses the current DPDK documentation retrieved through Context7 from the official
`/dpdk/dpdk` source and the DPDK 25.11 guides. Relevant upstream references:

- [DPDK Linux getting started guide](https://doc.dpdk.org/guides/linux_gsg/)
- [DPDK devbind tool guide](https://doc.dpdk.org/guides/tools/devbind.html)
- [DPDK sample applications guide](https://doc.dpdk.org/guides/sample_app_ug/)
- [DPDK API reference](https://doc.dpdk.org/api/)

## Existing Project Evidence

The repo already has kernel-networking and systems evidence that should stay ahead of speculative
DPDK work:

| Area | Existing evidence | What it proves |
|---|---|---|
| Socket path | `docs/socket_profiling.md`, `results/socket_profile_loopback.txt` | Loopback syscall/rusage shape for the TCP gateway path. |
| Multi-client serving | `docs/socket_gateway.md`, `results/socket_load_summary.txt` | Threaded vs epoll connection-scaling shape under bounded local load. |
| UDP pressure | `results/socket_stress_summary.txt` | Receive-buffer-driven drop behavior for bursty loopback UDP. |
| CPU locality | `docs/linux_performance.md`, `results/numa_affinity_study.txt` | Host-classified CPU-affinity/NUMA evidence, often constrained. |
| False sharing | `docs/concurrency_model.md`, `results/false_sharing_study.txt` | Benchmark-only SPSC cursor sharing shape, not production throughput. |
| Storage locality | `docs/pool_backed_storage.md`, `results/pool_backed_storage.txt` | Storage-layout tradeoffs for the matching path, independent of NIC I/O. |
| Durability/recovery | `docs/persistence.md`, `results/recovery_benchmarks.txt` | Recovery and persistence costs independent of packet ingress. |

The DPDK question is therefore not "make the simulator fast." It is: what would need to change at
the packet boundary, and what evidence would be required before saying anything about it?

## What DPDK Would Change

The current gateway accepts TCP frames through the kernel socket stack. A DPDK path would instead
move packet RX/TX into user space:

```text
NIC queue -> DPDK poll loop -> frame parser -> existing Session / Gateway -> response builder -> DPDK TX
```

The deterministic engine, integer-tick pricing, risk checks, event log, replay, and OCaml
differential tests do not need to change. The risky boundary is packet ingress/egress:

- Ethernet/IP/TCP handling is no longer provided by the kernel socket API unless a separate stack
  is used.
- Poll-mode drivers need dedicated CPU, memory, and NIC setup.
- Packet ownership uses DPDK mbufs and mempools, not `std::vector<std::byte>` socket buffers.
- Device queues, RSS, offloads, and timestamping become hardware-specific concerns.

For this repo, a credible DPDK prototype would be a narrow boundary experiment, not a replacement
for the exchange simulator.

## Environment Gate

Run the non-mutating environment check:

```bash
make dpdk-check
```

It writes [`results/dpdk_environment.txt`](../results/dpdk_environment.txt). The script never
reserves hugepages, loads kernel modules, binds NICs, or sends packets. It only records whether the
host appears able to build or run a DPDK prototype.

Evidence classes:

| Class | Meaning |
|---|---|
| `unsupported-host` | Non-Linux host. No DPDK packet-path evidence. |
| `linux-missing-dpdk` | Linux is present, but `pkg-config --exists libdpdk` fails. |
| `linux-dpdk-constrained` | DPDK development files are visible, but usable hugepages or devbind support is missing. |
| `linux-dpdk-build-ready` | DPDK build files, mounted/free hugepages, and devbind tooling are visible. This is still not packet evidence until a device is intentionally bound and a prototype runs. |

The current development host is expected to classify as `unsupported-host` when run on macOS.
That is a valid M48 outcome: it says the repo has research notes and a reproducible support check,
not a fake kernel-bypass measurement.

## Current DPDK Prerequisites

Current DPDK sample-app documentation centers on these steps:

1. Initialize EAL with `rte_eal_init(argc, argv)` before other DPDK APIs.
2. Pass EAL arguments for lcores, memory channels, hugepage location, and optional PCI allowlists.
3. Build with DPDK's Meson/pkg-config flow, typically through `pkg-config --cflags --libs libdpdk`
   for a small external sample.
4. Configure hugepages before running packet I/O.
5. Bind a real or virtual NIC to a DPDK-compatible driver such as `vfio-pci` before using it from
   a poll-mode driver.
6. Create mbuf pools, configure Ethernet ports, set RX/TX queue descriptors, start the device, and
   poll bursts in an explicit loop.

Those are environment and operations requirements, not C++ matching-engine requirements.

## Optional Prototype Policy

A prototype is optional for M48. Add one only when the environment can support it cleanly.

Minimum acceptable prototype, in order:

1. **Build-only EAL probe**: a tiny program that initializes EAL, prints lcore and hugepage-visible
   state, then exits. This needs `libdpdk` through pkg-config and a Linux host.
2. **No-NIC virtual-PMD probe**: only if DPDK supports a local virtual device on the host and the
   setup is documented.
3. **Packet-path probe**: only on a host with hugepages plus an intentionally bound device. It must
   record device, driver, queue count, EAL args, packet generator/load shape, and dirty-input
   provenance.

Do not add a prototype that:

- requires root-only host mutation without a clear opt-in;
- binds a developer's only network interface by default;
- records packet numbers without metadata;
- compares DPDK against kernel sockets using different workloads;
- changes core matching determinism or replay behavior.

## Measurement Policy

The only valid DPDK performance statement is one backed by a committed artifact from a clean run.
That artifact must state:

- whether it is build-only, virtual-device, loopback-like, or real-NIC evidence;
- exact DPDK version and EAL arguments;
- hugepage setup and driver binding state;
- NIC, driver, queue, offload, and CPU pinning details;
- workload shape and packet sizes;
- whether packet loss, drops, or backpressure occurred;
- source digest and dirty-input state;
- why the result is or is not comparable to the existing kernel socket artifacts.

Until those fields exist, DPDK remains research context only.

## Non-Goals

- No production-networking claim.
- No kernel-bypass speedup claim.
- No HFT or venue-certification language.
- No replacement of the deterministic engine with a packet-processing benchmark.
- No real-market connectivity.

M48 should improve networking literacy and evidence discipline. It should not turn the project into
a DPDK tutorial or a fake low-latency trading system.

# NIC Offload and Low-Latency Networking Study

M49 is a late-stage networking study. It records what would have to be true before Quant Systems
Lab could claim anything about NIC offloads, RSS, hardware timestamping, or vendor-specific
kernel-bypass stacks. On the current macOS development host, this milestone is research and
environment classification only.

Primary references used for this study:

- [Linux timestamping documentation](https://docs.kernel.org/networking/timestamping.html)
- [Linux networking scaling documentation](https://docs.kernel.org/networking/scaling.html)
- [Linux segmentation offloads documentation](https://docs.kernel.org/networking/segmentation-offloads.html)
- [ethtool(8) manual](https://man7.org/linux/man-pages/man8/ethtool.8.html)
- [AMD Onload User Guide](https://docs.amd.com/r/en-US/ug1586-onload-user)
- [NVIDIA MLNX_OFED time-stamping documentation](https://networking-docs.nvidia.com/mlnxofedswum/23070512/time-stamping)
- [OpenOnload source tree](https://github.com/Xilinx-CNS/onload)

Context7 did not return an official `ethtool` or Linux-kernel documentation corpus for this query;
the study therefore uses kernel, man-page, and vendor primary sources directly.

## Existing Evidence Boundary

This repo already has measured or classified evidence for adjacent layers:

| Area | Existing artifact | Boundary |
|---|---|---|
| Kernel socket path | `docs/socket_profiling.md`, `results/socket_profile_loopback.txt` | Loopback TCP/UDP syscall and rusage shape, not real NIC latency. |
| Socket pressure | `results/socket_load_summary.txt`, `results/socket_stress_summary.txt` | Local connection scaling and UDP burst/drop behavior. |
| CPU locality | `results/numa_affinity_study.txt` | Host-classified affinity and scheduler evidence. |
| False sharing | `results/false_sharing_study.txt` | Benchmark-only queue cursor contention. |
| DPDK | `docs/dpdk_research.md`, `results/dpdk_environment.txt` | DPDK environment gate, not packet-path evidence. |

M49 does not replace any of that. It defines what NIC/offload evidence would need to look like and
records whether the current host can even inspect relevant NIC capabilities.

## Non-Mutating Capability Check

Run:

```bash
make nic-offload-check
```

The target writes [`results/nic_offload_environment.txt`](../results/nic_offload_environment.txt).
It is intentionally read-only. It does not:

- enable or disable TSO, GSO, GRO, checksum, VLAN, or LRO offloads;
- change RSS indirection tables, hash keys, queue counts, or channel counts;
- configure hardware timestamp filters;
- change driver bindings, IRQ affinity, CPU affinity, or XPS/RPS masks;
- run a packet generator or latency benchmark.

On Linux, the script inspects non-loopback interfaces by default. To restrict the probe:

```bash
QSL_NIC_DEVICES="eth0 ens5f0" make nic-offload-check
```

Evidence classes:

| Class | Meaning |
|---|---|
| `unsupported-host` | Non-Linux host. No NIC offload/timestamping evidence. |
| `linux-missing-ethtool` | Linux sees network devices, but `ethtool` is unavailable. |
| `linux-no-observable-nic` | Linux host has no requested or non-loopback network device visible. |
| `linux-readonly-capability-observation` | Linux device capabilities were inspected read-only. This is still not latency evidence. |

If `QSL_NIC_DEVICES` names only absent interfaces, the artifact stays in the
`linux-no-observable-nic` class and lists the missing names instead of treating the request as a
successful capability observation.

The current macOS development host should classify as `unsupported-host`. That is a valid M49
result because the milestone is research-heavy unless suitable NIC hardware exists.

## What The Kernel Exposes

The Linux timestamping API separates timestamp generation from timestamp reporting. The kernel
documents `SO_TIMESTAMPING` as supporting receive and transmit timestamps from multiple sources,
including hardware. That is a capability path, not a guarantee that a particular NIC/driver can
produce useful hardware timestamps for this workload.

The Linux networking scaling documentation describes RSS as NIC-side steering of flows across
receive queues, with the receive queue chosen from a hash and indirection table. RPS, RFS, and XPS
are kernel/software steering mechanisms around that hardware boundary. For QSL, the relevant
question is not "does RSS exist?" but whether a measured experiment fixes the queue count, RSS
configuration, IRQ/CPU placement, packet flow shape, and receiver ownership model.

The Linux segmentation-offloads documentation covers TSO, GSO, GRO, tunnel offloads, and related
mechanisms. These features can reduce per-packet CPU work in throughput workloads, but they can
also change what a packet-level latency experiment is actually measuring. A QSL artifact must
record the exact feature state before interpreting any packet-path number.

`ethtool` is the normal Linux utility for querying and controlling wired Ethernet driver and
hardware settings. M49 uses only query forms such as driver info, offload feature listing, channel
listing, RSS indirection/hash display, and timestamping capability display.

## Vendor-Specific Context

### AMD Solarflare / Onload

OpenOnload is a user-level networking stack associated with AMD Solarflare adapters. Its current
source tree lists specific supported AMD Solarflare adapter families for native acceleration and
also documents AF_XDP-oriented paths for other adapters. The relevant QSL takeaway is that "Onload"
is not a generic switch that proves low latency on arbitrary hardware. Any credible artifact would
need adapter model, driver, Onload version, mode, timestamp configuration, CPU placement, and
packet workload.

AMD's Onload user guide has dedicated timestamping sections. Treat those as vendor-specific
capabilities that still require host validation before use in this repo.

### NVIDIA / Mellanox ConnectX

NVIDIA MLNX_OFED documentation describes hardware/software timestamping flags through the Linux
`SO_TIMESTAMPING` path and net-device timestamp configuration. As with Solarflare, this is a
hardware/driver capability boundary. A ConnectX result would need to identify the adapter, firmware,
driver stack, timestamp mode, RSS/queue layout, and measurement method.

### DPDK And Other Kernel-Bypass Paths

DPDK remains covered by the M48 notes. M49 only adds NIC/offload context around RSS, timestamping,
and vendor stacks. AF_XDP, Onload, DPDK, and vendor verbs can all be relevant research topics, but
none of them is evidence until a committed artifact records environment, device, configuration,
workload, and results.

## Measurement Standard

A future hardware-specific M49 measurement must include:

- NIC vendor, model, PCI address, firmware, driver, and kernel;
- whether the path is kernel sockets, AF_XDP, DPDK, Onload, or another user-space path;
- exact `ethtool -k`, `ethtool -l`, `ethtool -x`, and `ethtool -T` output or equivalent state;
- RSS queue count, indirection/hash configuration, IRQ/CPU affinity, and NUMA placement;
- timestamp source, clock synchronization method, PHC/PTP state, and whether timestamps are raw,
  system-transformed, software, or hardware;
- packet sizes, protocol, flow count, offered load, drop/backpressure behavior, and peer topology;
- source digest, dirty-input state, command line, and host metadata;
- a statement explaining whether the result is comparable to existing loopback socket artifacts.

Without those fields, M49 remains research/context only.

## Non-Goals

- No production-networking claim.
- No offload speedup or latency claim without measured hardware evidence.
- No claim that RSS or timestamping changes matching-engine determinism.
- No driver binding, offload toggling, or timestamp filter changes by default.
- No real-market connectivity.

The useful M49 signal is disciplined networking literacy: knowing which hardware/software boundary
matters and refusing to convert a checklist of NIC terms into fake performance evidence.

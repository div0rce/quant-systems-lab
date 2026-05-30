# Quant Systems Lab

A deterministic C++20 exchange simulator with a binary order gateway, price-time-priority matching engine, market-data publisher, append-only event log, replay/recovery path, and reproducible performance benchmarks.

This is a portfolio systems project for low-latency and quant SWE recruiting. It is not a production exchange and does not connect to real markets.

## Build

```bash
make configure
make build
make test
```

## Requirements

- C++20 compiler (Clang or GCC)
- CMake >= 3.24
- Ninja

## Project structure

```text
include/qsl/   — public headers
src/           — implementation
tests/         — unit and integration tests
docs/          — design documentation
data/          — synthetic test data
results/       — benchmark outputs (after M11)
```

## Development

```bash
make check     # format check + build + test
make fmt       # apply clang-format
make asan      # build and test with AddressSanitizer
make bench     # build release + run benchmarks -> results/latest.txt
```

## Benchmarks

These are **single-process synthetic microbenchmarks** produced by the committed harness
(`make bench`) — hot-cache, in-process, Release build. They **exclude** network I/O, disk
`fsync`, the kernel/socket path, allocator tuning, CPU pinning, and any production deployment
concern. They are **not** production exchange throughput or end-to-end latency, and they are
hardware-, compiler-, and build-dependent. They are useful for regression detection and
honest order-of-magnitude framing only.

The run below is one machine: arm64 / Apple clang 17 / Release / fixed seed 42. Full output
and metadata are in [`results/latest.txt`](results/latest.txt); methodology and caveats in
[docs/benchmarking.md](docs/benchmarking.md) and [docs/linux_performance.md](docs/linux_performance.md).

| Scenario (synthetic, in-process) | Measured on this run |
|---|---|
| Order book add/modify/cancel | ~68 ns/op |
| Protocol `NewOrder` encode+decode | ~20 ns/op |
| Gateway session, crossing order with fill | ~204 ns/op |
| Matching-engine flow (apply) | ~104 ns/command |
| Replay from command log | ~118 ns/command |

Reproduce with `make bench` (numbers will differ by machine).

## Status

Under active development. See [MILESTONES.md](MILESTONES.md) for the build plan.

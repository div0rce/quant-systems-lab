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
```

## Status

Under active development. See [MILESTONES.md](MILESTONES.md) for the build plan.

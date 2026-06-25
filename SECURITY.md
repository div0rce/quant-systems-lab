# Security Policy

This is a **systems lab / research portfolio project**, not a hardened production service. There
is **no bug bounty** and no security SLA. Please calibrate expectations accordingly.

## Local services are unauthenticated and loopback-only

The demo network components are intentionally minimal:

- `qsl-gateway` (TCP order entry) and `qsl-mdfeed` (UDP market data) have **no authentication**
  and bind to `127.0.0.1` only.
- They are for local demonstration. **Do not expose `qsl-gateway` or `qsl-mdfeed` to untrusted
  networks**, and do not run them on a shared or public interface.
- There is no TLS, access control, or rate limiting. The acceptors do have bounded resilience: an
  optional connection cap, survival of transient `accept()` errors and fd exhaustion, and `EINTR`
  retry on read/write, but this is robustness hardening, not DoS protection. Malformed input is
  handled by disconnecting the peer, not by recovering the stream.

## Reporting

If you find a memory-safety or correctness issue (e.g. something ASan/UBSan or the differential
tests would have caught), please open a GitHub issue, or contact the maintainer if you consider
it sensitive. Include a minimal reproducer where possible, the repo has tooling
(`qsl-export-stream`, the shrinker) for producing small deterministic fixtures.

## Honest scope

The project makes no production, exchange-grade, low-latency, or trading-profitability claims.
Its security posture matches its purpose: a deterministic, inspectable, locally-run simulator.

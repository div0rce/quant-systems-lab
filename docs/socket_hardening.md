# Linux socket hardening

This documents the defensive posture of the gateway/feed socket code: what is hardened, what is
intentionally *not* hardened, and why. It is the safety/robustness counterpart to
[`socket_gateway.md`](socket_gateway.md) (design) and
[`socket_profiling.md`](socket_profiling.md) (measurement). The honest threat model lives in
[`SECURITY.md`](../SECURITY.md): this is a **loopback systems lab, not a hardened production
service.** Nothing here claims a production-networking stack.

## Posture summary

| Concern | Handling | Where |
|---------|----------|-------|
| Untrusted network exposure | Bind `127.0.0.1` only; no auth | `tcp_server`, `udp_feed` |
| Invalid bind host | Numeric IPv4 only; bad host fails startup, no `0.0.0.0` fallback | M9 |
| Malformed frame | Flag session for disconnect; server drops peer, never desyncs/crashes | `Session` |
| Partial read/write | Length-prefixed framing; reassemble on read; send-all loop on write | `Session` |
| Peer disconnect mid-write | `send(MSG_NOSIGNAL)` / `SO_NOSIGPIPE` so `SIGPIPE` can't kill the process | `Session` |
| Indefinite blocking recv | Bounded `SO_RCVTIMEO` on the UDP client | `udp_feed` |
| UDP burst loss | Detected via sequence gaps; receive-buffer sizing knob (below) | `udp_feed` |
| UDP transmit failure | Counted, not silently dropped (`UdpPublisher::send_failures()`) | `udp_feed` |
| Signal during read/write | `EINTR` retried (not treated as a disconnect) | `TcpServer`/`EpollServer` |
| Transient accept error | `EINTR`/`ECONNABORTED` retried; listener kept alive | `TcpServer`/`EpollServer` |
| FD exhaustion | `EMFILE`/`ENFILE` survived (back-off retry / listener disarm-rearm), not a teardown | `TcpServer`/`EpollServer` |
| Connection-count overload | Optional cap (`max_active_connections`) load-sheds at the cap | `TcpServer` |

The first five rows pre-date M30 (M9/M10); M30 adds the receive-buffer sizing knob and documents
the loss model and the things deliberately left out.

## Frame robustness (recap)

The wire is a byte *stream*, not a message stream. The `Session` accumulates bytes and only acts
on a frame once the 16-byte header plus its declared `body_len` have arrived, so a frame split
across reads is held until complete, and a frame with a bad header / bad body / unexpected type
flags the connection for disconnect rather than risking stream desync. The server drops that one
peer and keeps running. This is fuzz-tested under ASan/UBSan (see `docs/invariants.md`), so
malformed input is a *handled* condition, not undefined behaviour.

## Receive-buffer sizing (`SO_RCVBUF`)

UDP has no flow control and no retransmit. If a burst arrives faster than the receiver drains it,
the datagrams that don't fit in the socket's receive buffer are **dropped by the kernel**,
silently. The buffer size therefore bounds how much burst the receiver can absorb.

M30 adds an optional receive-buffer request to `UdpFeedClient`:

```cpp
explicit UdpFeedClient(std::uint16_t port, int recv_buffer_bytes = 0);
```

- `recv_buffer_bytes > 0` requests that `SO_RCVBUF`; `0` leaves the OS default.
- The kernel may round the request up (Linux roughly doubles it for bookkeeping) or clamp it to
  a system maximum (`net.core.rmem_max` on Linux), so the **effective** granted size is read back
  with `getsockopt` and exposed via `recv_buffer_bytes()`. The CLI prints it, so experiments
  report *requested vs granted* rather than assuming the request was honoured.
- `qsl-mdfeed subscribe <port> [count] [rcvbuf_bytes]` exposes the knob.

The [`make socket-stress`](socket_profiling.md#udp-socket-buffer--burst-loss-experiment-make-socket-stress)
experiment measures the effect: an undersized buffer drops datagrams under burst (measured as
`published − received`, which also captures end-of-burst tail drops the sequence-gap counter
misses), while the default and larger buffers absorb the same burst. This is the honest,
measured justification for the knob, not a guess.

### Loss model: detected, not recovered

The feed **detects** loss (the `SequenceTracker` reports forward gaps from the message sequence
numbers) but does **not recover** it: there is no retransmit request, no NAK, and no snapshot /
gap-fill channel. A receiver that sees a gap knows it missed messages; rebuilding the missed
state would require the snapshot/recovery path, which is out of scope for the feed. This is
stated plainly so the gap counter is not mistaken for reliability.

## Intentionally out of scope (and why)

- **Event-driven load evidence.** M34 added a Linux `epoll` gateway architecture prototype and M35
  added bounded multi-client loopback load evidence. The portable TCP path now uses threaded
  per-connection workers; the artifacts remain constrained loopback evidence, not production
  capacity claims.
- **`io_uring`.** Discussed only. It could reduce syscall overhead on the gateway path, but it is
  a substantial, kernel-version-sensitive dependency and is not justified by any measured
  bottleneck here. No `io_uring` code exists; none is claimed.
- **TLS / authentication / authorization.** None. The services are loopback-only demos. Do not
  expose them on a routable interface (see `SECURITY.md`).
- **Connection caps.** Implemented as an opt-in `TcpServer` knob (`max_active_connections`, default
  `0` = unbounded): at the cap a freshly accepted connection is closed (load-shed) rather than
  spawning another worker. See the posture table above.
- **Idle-peer timeouts, rate limiting.** Not implemented. Heartbeats are a liveness round-trip only;
  the gateway does not yet time out idle peers. These are reasonable future hardening steps,
  explicitly not done today.
- **`SO_REUSEADDR` / rapid rebind.** Not set; the profiling scripts dodge `TIME_WAIT` by using
  separate ports per pass instead of forcing address reuse.

## Reproduce

```bash
make socket-stress     # UDP receive-buffer / burst-loss experiment (Linux or macOS)
make profile-io        # gateway syscall/rusage profile (Linux only)
```

See [`socket_profiling.md`](socket_profiling.md) for how to read the artifacts and the full
limitations (loopback only, `strace` perturbation, synthetic flow, hardware dependence).

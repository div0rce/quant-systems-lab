# ADR 0010: Linux epoll Gateway Prototype

## Status

Accepted

## Context

M9 intentionally used a blocking one-connection-at-a-time `TcpServer` so protocol/session
semantics could be proven without event-loop complexity. M30 profiled and hardened that socket
path, then deferred event-driven serving because Linux-only `epoll` code needed its own tested
milestone.

The pure `Session` layer already owns framing, malformed-input handling, risk dispatch, and
response encoding. Rewriting those semantics for an event loop would be unnecessary and risky.

## Decision

M34 adds `EpollServer`, a Linux-only transport prototype. It uses one `epoll` loop, nonblocking
`accept4`, nonblocking client sockets, per-client outbound buffers, and one `Session` per
connection. The default `qsl-gateway` path remains the portable blocking `TcpServer`; Linux users
can opt into the prototype with:

```bash
./build/dev/qsl-gateway 9009 --epoll
```

The epoll path handles readiness and buffering only. It does not change order-gateway risk logic,
matching, protocol codecs, or session semantics.

Per-client response buffering is bounded. The epoll transport applies a soft high-water mark that
stops reading from a client until pending output drains, and a hard cap enforced through a bounded
`Session` append path. High-fanout `NewOrder` responses are previewed against current engine state
before they reach the gateway, so an over-cap response drops the connection without appending a
partial response and without mutating engine state.

Disconnect handling distinguishes socket errors from peer hangups: `EPOLLERR` closes immediately,
while `EPOLLHUP` is honored only after any already-readable `EPOLLIN` bytes have been drained into
the session. Hard-cap overflow closes immediately when the over-cap frame appended nothing; if the
same read already queued replies for earlier accepted frames, reads are disabled and the bounded
pending replies are flushed before close.

Each client event stores a per-connection generation token with the fd. If a closed fd is reused
for a new connection while stale events from the old connection remain in the same `epoll_wait`
batch, the generation mismatch makes those stale events no-ops. Once a session is closing
(`close_after_flush`), the server no longer re-arms `EPOLLIN`; it only flushes pending replies and
then closes.

## Consequences

The repo now has a real event-driven multi-client gateway architecture without adopting
thread-per-connection design. Nonblocking `EAGAIN` / `EWOULDBLOCK` and partial writes are handled
as transport backpressure through retry-on-readiness, not as protocol errors.

This remains a simulator prototype:

- no production-capacity or low-latency claim;
- no TLS/auth/rate limiting;
- no `io_uring`;
- no multi-client load or socket-pressure benchmark in M34.

M35 owns load and pressure testing for the event-driven path.

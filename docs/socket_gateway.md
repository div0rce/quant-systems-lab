# Socket Gateway

The TCP order gateway exposes the in-process `OrderGateway` (M5) over a stream socket using
the M2 binary protocol. It is split into two pieces:

- **`Session`** (`include/qsl/gateway/session.hpp`) — a *pure* byte processor with no socket
  calls. It buffers inbound bytes, frames whole messages, drives the `OrderGateway`, and
  returns response bytes. This is where all protocol logic lives, so it is fully unit-tested
  deterministically.
- **`TcpServer`** (`include/qsl/gateway/tcp_server.hpp`) — a thin POSIX-socket transport:
  `serve_connection(fd)` runs a `Session` over one connected socket; `run(host, port)` binds,
  listens, and accepts connections one at a time.

## Message flow

```text
client -> NewOrder/CancelOrder/Heartbeat -> Session -> OrderGateway -> MatchingEngine
client <- Ack / Reject / Fill / HeartbeatAck <- Session
```

- A new order is acknowledged with `Ack` (carrying the accepted sequence number), followed by
  a `Fill` per trade.
- A risk-rejected (but well-formed) order returns a `Reject` with a `RejectReason`; the
  connection stays open.
- A `Heartbeat` is answered with a `HeartbeatAck` echoing the client's token.

## Frame boundaries and partial reads/writes

The wire is a byte stream, not a message stream. `Session` accumulates bytes in a buffer and
only processes a frame once the 16-byte header plus the declared `body_len` are present; a
frame split across multiple `read()`s is held until complete. Outbound responses are written
with a write-all loop that tolerates partial `write()`s.

## Malformed frames

- A frame with a valid header but an undecodable body, or an unexpected message type, flags
  the session for disconnect (the server drops the peer) rather than risk stream desync.
- A header that fails to decode (bad version, unknown type, oversized body) cannot be reframed
  safely, so the session is likewise flagged for disconnect.
- In all cases the server does not crash; it simply stops serving the misbehaving peer.

## Disconnect and heartbeat

- Graceful disconnect: when the peer closes its write side, `read()` returns 0 (EOF) and the
  server finishes serving and closes the connection.
- Heartbeats are a liveness round-trip only; the gateway does not yet time out idle peers.

## Why it is intentionally simple

A single-threaded accept-and-serve loop (one connection at a time) keeps the code easy to
reason about and avoids concurrency bugs. The goal is a credible, debuggable systems
demonstration, not a production matching venue, so there is no thread pool, no epoll/kqueue
event loop, and no TLS.

## Security

There is **no authentication or authorization**. The server binds to **`127.0.0.1` only**, so
it is reachable only from the local machine. Do not expose it on a routable interface or an
untrusted network — it accepts and acts on any order from any local connection. This is a
local simulator, not a real venue.

## Local demo

In one terminal, start the gateway (default port 9009):

```bash
make build
./build/dev/qsl-gateway 9009
```

In another terminal, run the client (sends a `NewOrder` and a `Heartbeat`, prints replies):

```bash
./build/dev/qsl-client 9009
# responses:
#   Ack order=1 seq=1
#   HeartbeatAck token=42
```

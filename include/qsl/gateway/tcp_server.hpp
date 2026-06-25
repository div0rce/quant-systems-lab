#pragma once

#include "qsl/gateway/order_gateway.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace qsl::gateway {

struct TcpServerOptions {
    // Maximum bytes a single connection may retain for responses generated from one read. 0
    // disables the cap. The default matches the epoll transport hard cap.
    std::size_t max_response_bytes = 8U << 20; // 8 MiB
    // listen(2) backlog used by the portable TCP server. Kept configurable so load tests can
    // normalize blocking/threaded and epoll transports.
    int listen_backlog = 128;
    // Test/embedding hook: 0 means serve indefinitely; otherwise return after accepting this many
    // connections, once their worker threads have drained and joined.
    std::size_t max_accepts = 0;
    // Admission control: cap on concurrently-served connections (worker threads). 0 disables the
    // cap. With one worker thread per connection, an unbounded count lets many slow/idle clients
    // exhaust threads/fds/memory; at the cap, a freshly accepted connection is closed immediately
    // (load-shed) rather than spawning another worker.
    std::size_t max_active_connections = 0;
};

/// Minimal threaded TCP front end for the order gateway. It binds to a host/port (intended for
/// localhost), accepts connections in one thread, and serves each connection on a worker thread.
/// Gateway mutation is serialized internally so the deterministic matching engine remains
/// single-owner. There is no authentication, this is a local simulator, not a venue.
class TcpServer {
  public:
    explicit TcpServer(OrderGateway &gateway, TcpServerOptions options = {})
        : gateway_(gateway), options_(options) {}

    /// Run a Session over one already-connected stream socket until EOF, peer logout, or
    /// error. The caller owns the descriptor's lifetime. Exposed for testing over a
    /// socketpair/loopback without bind/listen.
    void serve_connection(int fd);

    /// Serve an already-bound/listening TCP socket. The caller owns the descriptor's lifetime.
    /// Exposed for tests so they can bind an ephemeral port without changing server state.
    bool serve_listen_socket(int listen_fd);

    /// Bind host:port, listen, and serve accepted connections. Blocks until an accept error, or
    /// until max_accepts is reached when configured. Returns false if socket setup fails.
    bool run(const std::string &host, std::uint16_t port);

  private:
    OrderGateway &gateway_;
    TcpServerOptions options_;
    std::mutex gateway_mutex_;
};

} // namespace qsl::gateway

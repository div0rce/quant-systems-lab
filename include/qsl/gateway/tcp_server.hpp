#pragma once

#include "qsl/gateway/order_gateway.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace qsl::gateway {

struct TcpServerOptions {
    // Maximum bytes a single connection may retain for responses generated from one read. 0
    // disables the cap. The default matches the epoll transport hard cap.
    std::size_t max_response_bytes = 8U << 20; // 8 MiB
};

/// Minimal single-threaded TCP front end for the order gateway. It binds to a host/port
/// (intended for localhost), accepts one connection at a time, and runs a Session over the
/// stream. There is no authentication — this is a local simulator, not a venue.
class TcpServer {
  public:
    explicit TcpServer(OrderGateway &gateway, TcpServerOptions options = {})
        : gateway_(gateway), options_(options) {}

    /// Run a Session over one already-connected stream socket until EOF, peer logout, or
    /// error. The caller owns the descriptor's lifetime. Exposed for testing over a
    /// socketpair/loopback without bind/listen.
    void serve_connection(int fd);

    /// Bind host:port, listen, and serve accepted connections one at a time. Blocks until
    /// an accept error. Returns false if socket setup fails.
    bool run(const std::string &host, std::uint16_t port);

  private:
    OrderGateway &gateway_;
    TcpServerOptions options_;
};

} // namespace qsl::gateway

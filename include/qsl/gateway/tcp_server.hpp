#pragma once

#include "qsl/gateway/order_gateway.hpp"

#include <cstdint>
#include <string>

namespace qsl::gateway {

/// Minimal single-threaded TCP front end for the order gateway. It binds to a host/port
/// (intended for localhost), accepts one connection at a time, and runs a Session over the
/// stream. There is no authentication — this is a local simulator, not a venue.
class TcpServer {
  public:
    explicit TcpServer(OrderGateway &gateway) : gateway_(gateway) {}

    /// Run a Session over one already-connected stream socket until EOF, peer logout, or
    /// error. The caller owns the descriptor's lifetime. Exposed for testing over a
    /// socketpair/loopback without bind/listen.
    void serve_connection(int fd);

    /// Bind host:port, listen, and serve accepted connections one at a time. Blocks until
    /// an accept error. Returns false if socket setup fails.
    bool run(const std::string &host, std::uint16_t port);

  private:
    OrderGateway &gateway_;
};

} // namespace qsl::gateway

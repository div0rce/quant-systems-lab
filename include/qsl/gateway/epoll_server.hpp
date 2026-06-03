#pragma once

#include "qsl/gateway/order_gateway.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qsl::gateway {

struct EpollServerOptions {
    std::size_t max_events = 64;
    int wait_timeout_ms = 50;
    // Per-connection outbound high-water mark. While a client's pending response bytes reach this,
    // the server stops reading more requests from it (drops EPOLLIN) so a peer that stops reading
    // its responses cannot make the gateway buffer unbounded output; reads resume once the backlog
    // drains below the mark. This bounds per-connection memory under backpressure.
    std::size_t max_outbuf_bytes = 1U << 20; // 1 MiB
};

/// Linux epoll-based TCP front end for the order gateway. It is a transport prototype:
/// each connection still owns a deterministic Session, while one event loop multiplexes
/// accept/read/write readiness for multiple clients.
class EpollServer {
  public:
    explicit EpollServer(OrderGateway &gateway) : gateway_(gateway) {}

    [[nodiscard]] static bool supported() noexcept;

    void request_stop() noexcept { stop_requested_.store(true, std::memory_order_release); }

    /// Serve an already-bound/listening TCP socket. The caller owns the descriptor lifetime.
    /// Exposed for tests so they can bind an ephemeral port without changing server state.
    bool serve_listen_socket(int listen_fd, EpollServerOptions options = {});

    /// Bind host:port, listen, and serve clients until request_stop() or an unrecoverable
    /// setup error. Returns false if the platform or socket setup does not support epoll.
    bool run(const std::string &host, std::uint16_t port, EpollServerOptions options = {});

  private:
    OrderGateway &gateway_;
    std::atomic<bool> stop_requested_{false};
};

} // namespace qsl::gateway

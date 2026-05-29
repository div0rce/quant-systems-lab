#pragma once

#include "qsl/gateway/order_gateway.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace qsl::gateway {

/// Pure byte-level protocol session: no sockets. Buffers inbound bytes, decodes whole
/// frames, drives the in-process OrderGateway, and returns response bytes. A malformed
/// frame (bad header or undecodable body) flags the session for disconnect rather than
/// risking stream desync; a risk-rejected (but well-formed) order yields a Reject.
class Session {
  public:
    explicit Session(OrderGateway &gateway) : gateway_(gateway) {}

    /// Consume inbound bytes; return any response bytes to send back.
    [[nodiscard]] std::vector<std::byte> on_bytes(std::span<const std::byte> input);

    /// True once the peer should be disconnected (malformed frame seen).
    [[nodiscard]] bool logged_out() const noexcept { return logged_out_; }

  private:
    void process_frame(std::span<const std::byte> frame, std::vector<std::byte> &out);

    OrderGateway &gateway_;
    std::vector<std::byte> inbuf_;
    bool logged_out_ = false;
};

} // namespace qsl::gateway

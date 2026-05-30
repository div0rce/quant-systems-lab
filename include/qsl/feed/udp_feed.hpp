#pragma once

#include "qsl/feed/market_data.hpp"
#include "qsl/feed/publisher.hpp"
#include "qsl/feed/sequence_tracker.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace qsl::feed {

/// Sends each market-data message as one UDP datagram (encoded with the binary protocol).
/// Plugs into MarketDataPublisher as a subscriber. UDP is connectionless and lossy — there
/// is no retransmit; receivers detect loss via the message sequence number.
class UdpPublisher : public MarketDataSubscriber {
  public:
    UdpPublisher(const std::string &host, std::uint16_t port);
    ~UdpPublisher() override;
    UdpPublisher(const UdpPublisher &) = delete;
    UdpPublisher &operator=(const UdpPublisher &) = delete;

    [[nodiscard]] bool good() const noexcept { return fd_ >= 0; }
    void on_market_data(const MarketDataMessage &msg) override;

  private:
    int fd_ = -1;
    std::uint32_t dest_addr_ = 0; // network byte order
    std::uint16_t dest_port_ = 0; // network byte order
};

/// Receives market-data datagrams, decodes them, and tracks sequence gaps.
class UdpFeedClient {
  public:
    explicit UdpFeedClient(std::uint16_t port); // binds 127.0.0.1:port (0 = ephemeral)
    ~UdpFeedClient();
    UdpFeedClient(const UdpFeedClient &) = delete;
    UdpFeedClient &operator=(const UdpFeedClient &) = delete;

    [[nodiscard]] bool good() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint64_t total_gaps() const noexcept { return total_gaps_; }

    /// Receive and decode one datagram, updating the gap tracker. `gap` is set to the number
    /// of messages missed immediately before this one. Returns std::nullopt on error or an
    /// undecodable datagram.
    std::optional<MarketDataMessage> receive(std::uint64_t &gap);

  private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    SequenceTracker tracker_;
    std::uint64_t total_gaps_ = 0;
};

} // namespace qsl::feed

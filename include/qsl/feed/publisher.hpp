#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/feed/market_data.hpp"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace qsl::feed {

using core::Price;
using core::SeqNo;
using core::SymbolId;
using engine::EngineEvent;
using engine::MatchingEngine;

/// Receives market-data messages from a publisher.
class MarketDataSubscriber {
  public:
    virtual ~MarketDataSubscriber() = default;
    virtual void on_market_data(const MarketDataMessage &msg) = 0;
};

/// Transforms the engine's event stream into market-data messages. Emits an MdTrade per
/// fill and an MdTopOfBook per symbol whose top of book changed, in engine-event order,
/// each tagged with a monotonic md sequence number that follows the engine sequence.
class MarketDataPublisher {
  public:
    void subscribe(MarketDataSubscriber &subscriber);

    /// Process the events produced by one engine command application, reading resulting
    /// top-of-book from `engine`. Call once per applied command for accurate per-command
    /// top-of-book updates.
    void publish(const MatchingEngine &engine, std::span<const EngineEvent> events);

    [[nodiscard]] SeqNo md_seq() const noexcept { return md_seq_; }

  private:
    struct Tob {
        std::optional<Price> bid;
        std::optional<Price> ask;
        bool operator==(const Tob &) const = default;
    };

    void emit(const MarketDataMessage &msg);

    std::vector<MarketDataSubscriber *> subscribers_;
    std::unordered_map<SymbolId, Tob> last_tob_;
    SeqNo md_seq_{0};
};

} // namespace qsl::feed

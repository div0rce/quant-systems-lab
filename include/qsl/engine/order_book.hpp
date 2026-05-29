#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/order.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace qsl::engine {

using core::TimeInForce;

/// Deterministic single-symbol limit order book with price-time priority.
/// No wall-clock dependence; matching iterates ordered price levels (best first)
/// and FIFO order queues within a level.
class OrderBook {
  public:
    /// Add a limit order. Crosses against the opposite side at resting prices,
    /// then rests any remainder if GTC (IOC discards the remainder).
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                 TimeInForce tif);

    /// Add a market order. Crosses best available liquidity until filled or the
    /// book is depleted; never rests.
    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity);

    /// Remove a resting order. Returns false if the id is not resting.
    bool cancel(OrderId id);

    /// Modify a resting order. A pure quantity reduction at the same price keeps
    /// time priority; a price change or quantity increase loses priority (and may
    /// cross). new_quantity == 0 cancels the order.
    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] QuantityTotal quantity_at(Side side, Price price) const;
    [[nodiscard]] std::size_t order_count() const;
    [[nodiscard]] bool contains(OrderId id) const;

  private:
    using Level = std::list<Order>;
    using BidMap = std::map<Price, Level, std::greater<Price>>; // best (highest) first
    using AskMap = std::map<Price, Level, std::less<Price>>;    // best (lowest) first

    struct Locator {
        Side side;
        Price price;
        Level::iterator it;
    };

    template <class OppMap>
    void match_against(OppMap &opposite, OrderId taker_id, bool taker_is_buy, Price limit,
                       bool is_market, Quantity &quantity, std::vector<Trade> &trades);

    void rest(OrderId id, Side side, Price price, Quantity quantity);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Locator> index_;
};

} // namespace qsl::engine

#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/order.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory_resource>
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
    // M32 storage-experiment switch. Baseline == operator new/delete
    // (std::pmr::new_delete_resource), byte-for-byte the pre-M32 behavior. Pooled routes the
    // book's node storage through a per-book std::pmr::unsynchronized_pool_resource. The choice is
    // fixed at construction and never changes matching semantics or determinism; it only changes
    // where nodes live in memory. (See docs/pool_backed_storage.md.)
    enum class Storage { Baseline, Pooled };

    explicit OrderBook(Storage storage = Storage::Baseline);

    // The book owns its memory resource and its pmr containers point into it, so it is pinned in
    // place: non-copyable and non-movable. MatchingEngine constructs books in place (map
    // try_emplace), so this is not a usability constraint in practice.
    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;
    OrderBook(OrderBook &&) = delete;
    OrderBook &operator=(OrderBook &&) = delete;

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

    // Aggregate resting quantity per price level, best price first.
    [[nodiscard]] std::vector<LevelView> bid_levels() const;
    [[nodiscard]] std::vector<LevelView> ask_levels() const;

  private:
    // pmr node storage so the resting-order list nodes (and the level/index map nodes) can be
    // drawn from a pooled memory resource (M32). The container API is identical to the std::
    // versions, so the matching logic below is unchanged.
    using Level = std::pmr::list<Order>;
    using BidMap = std::pmr::map<Price, Level, std::greater<Price>>; // best (highest) first
    using AskMap = std::pmr::map<Price, Level, std::less<Price>>;    // best (lowest) first

    struct Locator {
        Side side;
        Price price;
        Level::iterator it;
    };

    template <class OppMap>
    void match_against(OppMap &opposite, OrderId taker_id, bool taker_is_buy, Price limit,
                       bool is_market, Quantity &quantity, std::vector<Trade> &trades);

    void rest(OrderId id, Side side, Price price, Quantity quantity);
    Level &level_for(Side side, Price price);

    // Declared before the containers so resource_ is valid when they are constructed. `pool_` is
    // engaged only in Pooled mode; resource_ points at it, otherwise at the shared
    // new_delete_resource (the baseline path).
    std::optional<std::pmr::unsynchronized_pool_resource> pool_;
    std::pmr::memory_resource *resource_;
    BidMap bids_;
    AskMap asks_;
    std::pmr::unordered_map<OrderId, Locator> index_;
};

} // namespace qsl::engine

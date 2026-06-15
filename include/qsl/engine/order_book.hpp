#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/order.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
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
    // Order-book storage-experiment switch. Baseline == operator new/delete
    // (std::pmr::new_delete_resource), byte-for-byte the pre-M32 behavior. Pooled routes std::list,
    // std::map, and std::unordered_map node storage through a per-book
    // std::pmr::unsynchronized_pool_resource. IntrusivePooled replaces the per-price std::list
    // nodes with custom FIFO nodes backed by the M28 raw OrderPool experiment; price/index maps
    // remain standard containers. Contiguous (M47) replaces the level maps with a direct
    // price-indexed flat array plus occupancy bitmaps and stores each level's FIFO queue in a
    // contiguous vector; it can rest orders only inside the explicit price band below. The choice
    // is fixed at construction. Within their declared domain (any price for Baseline/Pooled,
    // available capacity for IntrusivePooled, the in-band price range for Contiguous) the modes
    // preserve matching semantics and determinism and only change where resting-order state lives
    // in memory; outside it, IntrusivePooled and Contiguous can refuse a GTC remainder
    // (`can_store_limit` / `can_apply_modify`) that the others would rest.
    enum class Storage { Baseline, Pooled, IntrusivePooled, Contiguous };

    // Explicit price-domain assumption for Storage::Contiguous: GTC remainders can rest only at
    // integer-tick prices inside [kContiguousMinPrice, kContiguousMaxPrice]. Out-of-band limit
    // prices may still *match* (the band constrains resting, not crossing); an order whose
    // remainder would rest out of band is refused through can_store_limit, exactly like
    // IntrusivePooled refuses on pool exhaustion.
    static constexpr Price kContiguousMinPrice = 1;
    static constexpr Price kContiguousMaxPrice = 1024;

    explicit OrderBook(Storage storage = Storage::Baseline);
    ~OrderBook();

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
    [[nodiscard]] std::size_t fill_count(Side taker_side, Price limit, bool is_market,
                                         Quantity quantity) const;
    [[nodiscard]] bool can_store_limit(Side side, Price price, Quantity quantity,
                                       TimeInForce tif) const;

    /// Whether a nonzero modify of resting order `id` can be applied by this storage mode.
    /// Contiguous storage refuses a reprice whose remainder would rest outside the price band;
    /// callers must check this *before* emitting a modify event so the event stream never
    /// reports a modify the book refused. Unknown ids report true (contains() owns that check).
    [[nodiscard]] bool can_apply_modify(OrderId id, Price new_price, Quantity new_quantity) const;

    // Aggregate resting quantity per price level, best price first.
    [[nodiscard]] std::vector<LevelView> bid_levels() const;
    [[nodiscard]] std::vector<LevelView> ask_levels() const;

    /// Every resting order in deterministic priority order: bid levels best (highest) first,
    /// then ask levels best (lowest) first, FIFO within each level. Re-adding the orders in
    /// this sequence into an empty book reproduces the same levels and intra-level time
    /// priority, so this is exactly the state a snapshot/restore path would have to persist.
    [[nodiscard]] std::vector<Order> resting_orders() const;

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

    struct MatchContext {
        OrderId taker_id;
        bool taker_is_buy;
        Price limit;
        bool is_market;
        Quantity &quantity;
        std::vector<Trade> &trades;
    };

    struct MatchQuery {
        bool taker_is_buy;
        Price limit;
        bool is_market;
        Quantity quantity;
    };

    struct MatchResult {
        std::vector<Trade> trades;
        Quantity remainder;
        bool accepted;
    };

    template <class OppMap> void match_against(OppMap &opposite, MatchContext &ctx);

    template <class OppMap>
    std::size_t count_matches(const OppMap &opposite, MatchQuery query) const;
    template <class ContainsFn, class MatchFn>
    static MatchResult match_incoming(OrderId id, Side side, Price limit, bool is_market,
                                      Quantity quantity, ContainsFn &&contains_fn,
                                      MatchFn &&match_fn);
    template <class OppMap, class FillLevelFn, class EraseEmptyFn>
    static void match_ordered_levels(OppMap &opposite, MatchContext &ctx,
                                     FillLevelFn &&fill_level_fn, EraseEmptyFn &&erase_empty_fn);
    template <class Index, class RemoveFn>
    static bool cancel_indexed_order(Index &index, OrderId id, RemoveFn &&remove_fn);
    template <class BaselineFn, class IntrusiveFn, class ContiguousFn>
    decltype(auto) dispatch_storage(BaselineFn &&baseline_fn, IntrusiveFn &&intrusive_fn,
                                    ContiguousFn &&contiguous_fn);
    template <class BaselineFn, class IntrusiveFn, class ContiguousFn>
    decltype(auto) dispatch_storage(BaselineFn &&baseline_fn, IntrusiveFn &&intrusive_fn,
                                    ContiguousFn &&contiguous_fn) const;
    template <class LevelMap>
    void erase_level_if_empty(LevelMap &levels, typename LevelMap::iterator level_it);
    template <class LevelMap> void erase_from_side(LevelMap &levels, const Locator &loc);

    void rest(OrderId id, Side side, Price price, Quantity quantity);
    Level &level_for(Side side, Price price);
    const Level *find_level(Side side, Price price) const;
    void match_baseline(Side side, MatchContext &ctx);
    void fill_front_order(Level &level, Price level_price, MatchContext &ctx);
    void fill_level(Level &level, Price level_price, MatchContext &ctx);
    void erase_resting_order(const Locator &loc);
    static bool fill_maker(MatchContext &ctx, OrderId maker_id, Quantity &maker_quantity,
                           Price level_price);
    static bool should_rest_limit(const MatchResult &result, TimeInForce tif) noexcept;
    static bool can_match_level(bool taker_is_buy, bool is_market, Price limit, Price level_price);
    static QuantityTotal level_quantity(const Level &level);
    struct IntrusiveStore;
    struct ContiguousStore;

    // Declared before the containers so resource_ is valid when they are constructed. `pool_` is
    // engaged only in Pooled mode; resource_ points at it, otherwise at the shared
    // new_delete_resource (the baseline path).
    std::optional<std::pmr::unsynchronized_pool_resource> pool_;
    std::pmr::memory_resource *resource_;
    BidMap bids_;
    AskMap asks_;
    std::pmr::unordered_map<OrderId, Locator> index_;
    std::unique_ptr<IntrusiveStore> intrusive_;
    std::unique_ptr<ContiguousStore> contiguous_;
};

} // namespace qsl::engine

#pragma once

#include "qsl/engine/order_book.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qsl::engine {

// M47 contiguous storage: price levels live in a flat array indexed directly by price tick
// (band [kContiguousMinPrice, kContiguousMaxPrice]); per-side occupancy bitmaps answer
// best-price queries; each level's FIFO queue is a contiguous vector with a head cursor and
// tombstoned cancels. The band constrains *resting* only — out-of-band limit prices still
// match — and an order whose remainder would rest out of band is refused via can_store_limit.
struct OrderBook::ContiguousStore {
    static constexpr Price kMinPrice = OrderBook::kContiguousMinPrice;
    static constexpr Price kMaxPrice = OrderBook::kContiguousMaxPrice;
    static constexpr std::size_t kBandWidth = static_cast<std::size_t>(kMaxPrice - kMinPrice + 1);
    static constexpr std::size_t kBitmapWords = (kBandWidth + 63) / 64;
    // A level compacts when tombstones outnumber live orders past this size, so churn at one
    // price cannot grow its queue without bound.
    static constexpr std::size_t kCompactMinSize = 16;

    struct FlatOrder {
        OrderId id;
        Quantity quantity; // 0 marks a tombstone; live orders always have quantity > 0
    };

    struct FlatLevel {
        std::vector<FlatOrder> orders;
        std::uint32_t head = 0; // index of the first live order while alive > 0
        std::uint32_t alive = 0;
    };

    struct Locator {
        Side side;
        Price price;
        std::uint32_t pos;
    };

    using Bitmap = std::array<std::uint64_t, kBitmapWords>;

    struct BookSide {
        std::vector<FlatLevel> levels{kBandWidth};
        Bitmap bits{};
    };

    BookSide bids;
    BookSide asks;
    std::unordered_map<OrderId, Locator> index;

    static bool in_band(Price price) noexcept { return price >= kMinPrice && price <= kMaxPrice; }
    static std::size_t slot_of(Price price) noexcept {
        return static_cast<std::size_t>(price - kMinPrice);
    }
    static Price price_of(std::size_t slot) noexcept {
        return kMinPrice + static_cast<Price>(slot);
    }

    BookSide &side_ref(Side side) { return side == Side::Buy ? bids : asks; }
    const BookSide &side_ref(Side side) const { return side == Side::Buy ? bids : asks; }

    static void set_bit(Bitmap &bits, std::size_t slot) noexcept {
        bits[slot / 64] |= std::uint64_t{1} << (slot % 64);
    }
    static void clear_bit(Bitmap &bits, std::size_t slot) noexcept {
        bits[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
    }

    // Lowest occupied slot at or above `from` (ask side best-first order), or nullopt.
    static std::optional<std::size_t> next_up(const Bitmap &bits, std::size_t from) noexcept {
        for (std::size_t word = from / 64; word < kBitmapWords; ++word) {
            std::uint64_t value = bits[word];
            if (word == from / 64) {
                value &= ~std::uint64_t{0} << (from % 64);
            }
            if (value != 0) {
                return word * 64 + static_cast<std::size_t>(std::countr_zero(value));
            }
        }
        return std::nullopt;
    }

    // Highest occupied slot at or below `from` (bid side best-first order), or nullopt.
    static std::optional<std::size_t> next_down(const Bitmap &bits, std::size_t from) noexcept {
        for (std::size_t word = from / 64 + 1; word-- > 0;) {
            std::uint64_t value = bits[word];
            if (word == from / 64 && (from % 64) != 63) {
                value &= (std::uint64_t{1} << ((from % 64) + 1)) - 1;
            }
            if (value != 0) {
                return word * 64 + (63 - static_cast<std::size_t>(std::countl_zero(value)));
            }
        }
        return std::nullopt;
    }

    static std::optional<std::size_t> best_slot(const BookSide &side, bool is_bid) noexcept {
        return is_bid ? next_down(side.bits, kBandWidth - 1) : next_up(side.bits, 0);
    }

    // The next occupied slot strictly worse than `slot` for the given side, or nullopt.
    static std::optional<std::size_t> next_worse(const Bitmap &bits, bool is_bid,
                                                 std::size_t slot) noexcept {
        if (is_bid) {
            return slot == 0 ? std::optional<std::size_t>{} : next_down(bits, slot - 1);
        }
        return slot + 1 >= kBandWidth ? std::optional<std::size_t>{} : next_up(bits, slot + 1);
    }

    // Visit occupied levels best-first; `fn(slot, level)` returns false to stop early.
    template <class Fn> void for_each_occupied(const BookSide &side, bool is_bid, Fn &&fn) const {
        auto slot = best_slot(side, is_bid);
        while (slot) {
            if (!fn(*slot, side.levels[*slot])) {
                return;
            }
            slot = next_worse(side.bits, is_bid, *slot);
        }
    }

    static void advance_head(FlatLevel &level) noexcept {
        while (level.head < level.orders.size() && level.orders[level.head].quantity == 0) {
            ++level.head;
        }
    }

    void clear_level(BookSide &side, std::size_t slot) {
        FlatLevel &level = side.levels[slot];
        level.orders.clear();
        level.head = 0;
        level.alive = 0;
        clear_bit(side.bits, slot);
    }

    // Rewrite the FIFO with live orders only (preserving time priority) and refresh their
    // locator positions.
    void compact_level(FlatLevel &level) {
        std::vector<FlatOrder> live;
        live.reserve(level.alive);
        for (std::size_t i = level.head; i < level.orders.size(); ++i) {
            if (level.orders[i].quantity > 0) {
                auto found = index.find(level.orders[i].id);
                if (found != index.end()) {
                    found->second.pos = static_cast<std::uint32_t>(live.size());
                }
                live.push_back(level.orders[i]);
            }
        }
        level.orders = std::move(live);
        level.head = 0;
    }

    void maybe_compact(FlatLevel &level) {
        const std::size_t dead = level.orders.size() - level.alive;
        if (level.orders.size() >= kCompactMinSize && dead > level.alive) {
            compact_level(level);
        }
    }

    bool rest(OrderId id, Side side, Price price, Quantity quantity) {
        if (!in_band(price)) {
            return false;
        }
        BookSide &book_side = side_ref(side);
        const std::size_t slot = slot_of(price);
        FlatLevel &level = book_side.levels[slot];
        if (level.alive == 0) {
            set_bit(book_side.bits, slot);
        }
        index[id] = Locator{side, price, static_cast<std::uint32_t>(level.orders.size())};
        level.orders.push_back(FlatOrder{id, quantity});
        ++level.alive;
        return true;
    }

    void fill_front_order(FlatLevel &level, Price level_price, OrderBook::MatchContext &ctx) {
        FlatOrder &maker = level.orders[level.head];
        if (OrderBook::fill_maker(ctx, maker.id, maker.quantity, level_price)) {
            index.erase(maker.id);
            --level.alive;
            advance_head(level);
        }
    }

    void fill_level(FlatLevel &level, Price level_price, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0 && level.alive > 0) {
            fill_front_order(level, level_price, ctx);
        }
    }

    void match_against(BookSide &opposite, bool opposite_is_bid, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0) {
            const auto slot = best_slot(opposite, opposite_is_bid);
            if (!slot) {
                return;
            }
            const Price level_price = price_of(*slot);
            if (!OrderBook::can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit,
                                            level_price)) {
                return;
            }
            FlatLevel &level = opposite.levels[*slot];
            fill_level(level, level_price, ctx);
            if (level.alive == 0) {
                clear_level(opposite, *slot);
            } else {
                maybe_compact(level); // fills leave a dead prefix; keep long-lived levels bounded
            }
        }
    }

    void match_opposite(Side side, OrderBook::MatchContext &ctx) {
        const bool taker_is_buy = side == Side::Buy;
        match_against(taker_is_buy ? asks : bids, !taker_is_buy, ctx);
    }

    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                 TimeInForce tif) {
        if (!can_store_limit(side, price, quantity, tif)) {
            return {};
        }
        OrderBook::MatchResult result = OrderBook::match_incoming(
            id, side, price, /*is_market=*/false, quantity, [&] { return contains(id); },
            [&](Side taker_side, OrderBook::MatchContext &ctx) {
                match_opposite(taker_side, ctx);
            });
        if (OrderBook::should_rest_limit(result, tif)) {
            static_cast<void>(rest(id, side, price, result.remainder));
        }
        return result.trades;
    }

    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity) {
        return OrderBook::match_incoming(
                   id, side, /*limit=*/0, /*is_market=*/true, quantity,
                   [&] { return contains(id); },
                   [&](Side taker_side, OrderBook::MatchContext &ctx) {
                       match_opposite(taker_side, ctx);
                   })
            .trades;
    }

    void remove_resting(const Locator &loc) {
        BookSide &book_side = side_ref(loc.side);
        const std::size_t slot = slot_of(loc.price);
        FlatLevel &level = book_side.levels[slot];
        level.orders[loc.pos].quantity = 0;
        --level.alive;
        if (level.alive == 0) {
            clear_level(book_side, slot);
            return;
        }
        if (loc.pos == level.head) {
            advance_head(level);
        }
        maybe_compact(level);
    }

    bool cancel(OrderId id) {
        return OrderBook::cancel_indexed_order(index, id,
                                               [&](const Locator &loc) { remove_resting(loc); });
    }

    // A same-price modify without a quantity increase reduces in place and keeps time priority;
    // anything else cancels and re-adds (which may cross).
    [[nodiscard]] bool reduces_in_place(const Locator &loc, Price new_price,
                                        Quantity new_quantity) const {
        const FlatOrder &order = side_ref(loc.side).levels[slot_of(loc.price)].orders[loc.pos];
        return new_price == loc.price && new_quantity <= order.quantity;
    }

    // Pre-gate for MatchingEngine::modify, mirroring can_store_limit for new orders: the engine
    // asks before emitting OrderModified, so a refused reprice never reaches the event stream.
    // The band constrains resting, so the only refusal is a cancel-and-re-add whose remainder
    // would rest at an out-of-band price.
    [[nodiscard]] bool can_apply_modify(OrderId id, Price new_price, Quantity new_quantity) const {
        const auto found = index.find(id);
        if (found == index.end()) {
            return true; // unknown ids are the caller's contains() check, not a refusal
        }
        if (new_quantity == 0) {
            return true; // cancel-via-modify never re-rests
        }
        if (reduces_in_place(found->second, new_price, new_quantity)) {
            return true;
        }
        if (in_band(new_price)) {
            return true;
        }
        return simulate_remainder(found->second.side, new_price, new_quantity) == 0;
    }

    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity) {
        std::vector<Trade> trades;
        const auto found = index.find(id);
        if (found == index.end()) {
            return trades;
        }
        // Defense in depth for direct OrderBook callers: engine paths are pre-gated by the same
        // query, so a refusal here keeps the original order intact instead of dropping it.
        if (!can_apply_modify(id, new_price, new_quantity)) {
            return trades;
        }
        const Locator loc = found->second;
        if (new_quantity == 0) {
            static_cast<void>(cancel(id));
            return trades;
        }
        if (reduces_in_place(loc, new_price, new_quantity)) {
            side_ref(loc.side).levels[slot_of(loc.price)].orders[loc.pos].quantity = new_quantity;
            return trades;
        }
        static_cast<void>(cancel(id));
        return add_limit(id, loc.side, new_price, new_quantity, TimeInForce::GTC);
    }

    // The remainder a limit order would have left after crossing the current opposite side.
    [[nodiscard]] Quantity simulate_remainder(Side side, Price limit, Quantity quantity) const {
        const bool taker_is_buy = side == Side::Buy;
        const BookSide &opposite = taker_is_buy ? asks : bids;
        for_each_occupied(opposite, !taker_is_buy, [&](std::size_t slot, const FlatLevel &level) {
            if (quantity == 0 || !OrderBook::can_match_level(taker_is_buy, /*is_market=*/false,
                                                             limit, price_of(slot))) {
                return false;
            }
            for (std::size_t i = level.head; quantity > 0 && i < level.orders.size(); ++i) {
                quantity -= std::min(quantity, level.orders[i].quantity); // tombstones add 0
            }
            return quantity > 0;
        });
        return quantity;
    }

    static std::size_t count_flat_level(const FlatLevel &level, Quantity &quantity) {
        std::size_t count = 0;
        for (std::size_t i = level.head; quantity > 0 && i < level.orders.size(); ++i) {
            if (level.orders[i].quantity > 0) {
                quantity -= std::min(quantity, level.orders[i].quantity);
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t count_flat_matches(const BookSide &opposite, bool opposite_is_bid,
                                                 OrderBook::MatchQuery query) const {
        std::size_t count = 0;
        for_each_occupied(opposite, opposite_is_bid, [&](std::size_t slot, const FlatLevel &level) {
            if (query.quantity == 0 ||
                !OrderBook::can_match_level(query.taker_is_buy, query.is_market, query.limit,
                                            price_of(slot))) {
                return false;
            }
            count += count_flat_level(level, query.quantity);
            return query.quantity > 0;
        });
        return count;
    }

    [[nodiscard]] bool can_store_limit(Side side, Price price, Quantity quantity,
                                       TimeInForce tif) const {
        if (tif == TimeInForce::IOC || in_band(price)) {
            return true;
        }
        return simulate_remainder(side, price, quantity) == 0;
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        const auto slot = best_slot(bids, /*is_bid=*/true);
        return slot ? std::optional<Price>{price_of(*slot)} : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        const auto slot = best_slot(asks, /*is_bid=*/false);
        return slot ? std::optional<Price>{price_of(*slot)} : std::nullopt;
    }

    static QuantityTotal level_total(const FlatLevel &level) {
        QuantityTotal total = 0;
        for (std::size_t i = level.head; i < level.orders.size(); ++i) {
            total += static_cast<QuantityTotal>(level.orders[i].quantity);
        }
        return total;
    }

    [[nodiscard]] QuantityTotal quantity_at(Side side, Price price) const {
        if (!in_band(price)) {
            return 0;
        }
        return level_total(side_ref(side).levels[slot_of(price)]);
    }

    [[nodiscard]] std::size_t order_count() const { return index.size(); }
    [[nodiscard]] bool contains(OrderId id) const { return index.find(id) != index.end(); }

    [[nodiscard]] std::size_t fill_count(Side taker_side, Price limit, bool is_market,
                                         Quantity quantity) const {
        if (taker_side != Side::Buy && taker_side != Side::Sell) {
            return 0;
        }
        const bool taker_is_buy = taker_side == Side::Buy;
        const BookSide &opposite = taker_is_buy ? asks : bids;
        return count_flat_matches(opposite, !taker_is_buy,
                                  OrderBook::MatchQuery{taker_is_buy, limit, is_market, quantity});
    }

    [[nodiscard]] std::vector<LevelView> collect_levels(const BookSide &side, bool is_bid) const {
        std::vector<LevelView> levels;
        for_each_occupied(side, is_bid, [&](std::size_t slot, const FlatLevel &level) {
            levels.push_back(LevelView{price_of(slot), level_total(level)});
            return true;
        });
        return levels;
    }

    [[nodiscard]] std::vector<LevelView> bid_levels() const {
        return collect_levels(bids, /*is_bid=*/true);
    }
    [[nodiscard]] std::vector<LevelView> ask_levels() const {
        return collect_levels(asks, /*is_bid=*/false);
    }

    void append_orders(const BookSide &side, bool is_bid, Side side_value,
                       std::vector<Order> &out) const {
        for_each_occupied(side, is_bid, [&](std::size_t slot, const FlatLevel &level) {
            for (std::size_t i = level.head; i < level.orders.size(); ++i) {
                if (level.orders[i].quantity > 0) {
                    out.push_back(Order{level.orders[i].id, side_value, price_of(slot),
                                        level.orders[i].quantity});
                }
            }
            return true;
        });
    }

    [[nodiscard]] std::vector<Order> resting_orders() const {
        std::vector<Order> out;
        out.reserve(index.size());
        append_orders(bids, /*is_bid=*/true, Side::Buy, out);
        append_orders(asks, /*is_bid=*/false, Side::Sell, out);
        return out;
    }
};

} // namespace qsl::engine

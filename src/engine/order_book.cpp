#include "qsl/engine/order_book.hpp"

#include <algorithm>

namespace qsl::engine {

// Baseline uses the shared new_delete_resource (operator new/delete -> identical to pre-M32).
// Pooled owns an unsynchronized_pool_resource; resource_ is set before the pmr containers (which
// hold polymorphic_allocators referencing it) are constructed. Per-price FIFO lists are explicitly
// constructed with resource_ when a new level is inserted.
OrderBook::OrderBook(Storage storage)
    : pool_(storage == Storage::Pooled
                ? std::optional<std::pmr::unsynchronized_pool_resource>(std::in_place)
                : std::nullopt),
      resource_(pool_.has_value() ? &pool_.value() : std::pmr::new_delete_resource()),
      bids_(resource_), asks_(resource_), index_(resource_) {}

bool OrderBook::can_match_level(bool taker_is_buy, bool is_market, Price limit, Price level_price) {
    if (is_market) {
        return true;
    }
    return taker_is_buy ? level_price <= limit : level_price >= limit;
}

void OrderBook::fill_front_order(Level &level, Price level_price, MatchContext &ctx) {
    Order &maker = level.front();
    const Quantity traded = std::min(ctx.quantity, maker.quantity);
    ctx.trades.push_back(Trade{ctx.taker_id, maker.id, level_price, traded});
    ctx.quantity -= traded;
    maker.quantity -= traded;
    if (maker.quantity == 0) {
        index_.erase(maker.id);
        level.pop_front();
    }
}

void OrderBook::fill_level(Level &level, Price level_price, MatchContext &ctx) {
    while (ctx.quantity > 0 && !level.empty()) {
        fill_front_order(level, level_price, ctx);
    }
}

template <class LevelMap>
void OrderBook::erase_level_if_empty(LevelMap &levels, typename LevelMap::iterator level_it) {
    if (level_it->second.empty()) {
        levels.erase(level_it);
    }
}

template <class OppMap> void OrderBook::match_against(OppMap &opposite, MatchContext &ctx) {
    while (ctx.quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin(); // best price on the opposite side
        const Price level_price = level_it->first;
        if (!can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit, level_price)) {
            break;
        }
        fill_level(level_it->second, level_price, ctx);
        erase_level_if_empty(opposite, level_it);
    }
}

template <class Level> std::size_t count_level_matches(const Level &level, Quantity &quantity) {
    std::size_t count = 0;
    for (auto order_it = level.begin(); quantity > 0 && order_it != level.end(); ++order_it) {
        const Order &maker = *order_it;
        const Quantity traded = std::min(quantity, maker.quantity);
        quantity -= traded;
        ++count;
    }
    return count;
}

template <class OppMap>
std::size_t OrderBook::count_matches(const OppMap &opposite, MatchQuery query) const {
    std::size_t count = 0;
    for (auto level_it = opposite.begin(); query.quantity > 0 && level_it != opposite.end();
         ++level_it) {
        const Price level_price = level_it->first;
        if (!can_match_level(query.taker_is_buy, query.is_market, query.limit, level_price)) {
            break;
        }
        count += count_level_matches(level_it->second, query.quantity);
    }
    return count;
}

OrderBook::Level &OrderBook::level_for(Side side, Price price) {
    if (side == Side::Buy) {
        auto [it, inserted] = bids_.emplace(price, Level{Level::allocator_type{resource_}});
        (void)inserted;
        return it->second;
    }
    auto [it, inserted] = asks_.emplace(price, Level{Level::allocator_type{resource_}});
    (void)inserted;
    return it->second;
}

void OrderBook::rest(OrderId id, Side side, Price price, Quantity quantity) {
    Level &level = level_for(side, price);
    level.push_back(Order{id, side, price, quantity});
    index_[id] = Locator{side, price, std::prev(level.end())};
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                        TimeInForce tif) {
    std::vector<Trade> trades;
    if (contains(id)) {
        return trades;
    }
    MatchContext ctx{id, side == Side::Buy, price, /*is_market=*/false, quantity, trades};
    if (side == Side::Buy) {
        match_against(asks_, ctx);
    } else {
        match_against(bids_, ctx);
    }
    if (quantity > 0 && tif == TimeInForce::GTC) {
        rest(id, side, price, quantity);
    }
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    std::vector<Trade> trades;
    if (contains(id)) {
        return trades;
    }
    MatchContext ctx{id, side == Side::Buy, /*limit=*/0, /*is_market=*/true, quantity, trades};
    if (side == Side::Buy) {
        match_against(asks_, ctx);
    } else {
        match_against(bids_, ctx);
    }
    return trades; // market orders never rest
}

template <class LevelMap> void OrderBook::erase_from_side(LevelMap &levels, const Locator &loc) {
    auto level_it = levels.find(loc.price);
    level_it->second.erase(loc.it);
    erase_level_if_empty(levels, level_it);
}

void OrderBook::erase_resting_order(const Locator &loc) {
    if (loc.side == Side::Buy) {
        erase_from_side(bids_, loc);
        return;
    }
    erase_from_side(asks_, loc);
}

bool OrderBook::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    const Locator loc = found->second;
    erase_resting_order(loc);
    index_.erase(found);
    return true;
}

std::vector<Trade> OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    std::vector<Trade> trades;
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return trades; // unknown order: no-op here; rejection is the risk layer's job (M5)
    }
    const Locator loc = found->second;
    if (new_quantity == 0) {
        cancel(id);
        return trades;
    }
    if (new_price == loc.price && new_quantity <= loc.it->quantity) {
        loc.it->quantity = new_quantity; // reduce in place, preserve time priority
        return trades;
    }
    // Price change or quantity increase loses priority: re-add at the tail (may cross).
    const Side side = loc.side;
    cancel(id);
    return add_limit(id, side, new_price, new_quantity, TimeInForce::GTC);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

QuantityTotal OrderBook::quantity_at(Side side, Price price) const {
    const Level *level = find_level(side, price);
    return level == nullptr ? 0 : level_quantity(*level);
}

const OrderBook::Level *OrderBook::find_level(Side side, Price price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? nullptr : &it->second;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? nullptr : &it->second;
}

QuantityTotal OrderBook::level_quantity(const Level &level) {
    QuantityTotal total = 0;
    for (const Order &o : level) {
        total += static_cast<QuantityTotal>(o.quantity);
    }
    return total;
}

std::size_t OrderBook::order_count() const {
    return index_.size();
}

bool OrderBook::contains(OrderId id) const {
    return index_.find(id) != index_.end();
}

std::size_t OrderBook::fill_count(Side taker_side, Price limit, bool is_market,
                                  Quantity quantity) const {
    if (taker_side == Side::Buy) {
        return count_matches(asks_, MatchQuery{/*taker_is_buy=*/true, limit, is_market, quantity});
    }
    if (taker_side == Side::Sell) {
        return count_matches(bids_, MatchQuery{/*taker_is_buy=*/false, limit, is_market, quantity});
    }
    return 0;
}

namespace {
template <class LevelMap> std::vector<LevelView> collect_levels(const LevelMap &book) {
    std::vector<LevelView> levels;
    levels.reserve(book.size());
    for (const auto &[price, orders] : book) {
        QuantityTotal total = 0;
        for (const Order &o : orders) {
            total += static_cast<QuantityTotal>(o.quantity);
        }
        levels.push_back(LevelView{price, total});
    }
    return levels;
}
} // namespace

std::vector<LevelView> OrderBook::bid_levels() const {
    return collect_levels(bids_);
}

std::vector<LevelView> OrderBook::ask_levels() const {
    return collect_levels(asks_);
}

} // namespace qsl::engine

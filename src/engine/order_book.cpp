#include "qsl/engine/order_book.hpp"

#include <algorithm>

namespace qsl::engine {

template <class OppMap>
void OrderBook::match_against(OppMap &opposite, OrderId taker_id, bool taker_is_buy, Price limit,
                              bool is_market, Quantity &quantity, std::vector<Trade> &trades) {
    while (quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin(); // best price on the opposite side
        const Price level_price = level_it->first;
        if (!is_market) {
            // Stop once the best resting price no longer crosses the limit.
            if (taker_is_buy && level_price > limit) {
                break;
            }
            if (!taker_is_buy && level_price < limit) {
                break;
            }
        }
        Level &level = level_it->second;
        while (quantity > 0 && !level.empty()) {
            Order &maker = level.front();
            const Quantity traded = std::min(quantity, maker.quantity);
            trades.push_back(Trade{taker_id, maker.id, level_price, traded});
            quantity -= traded;
            maker.quantity -= traded;
            if (maker.quantity == 0) {
                index_.erase(maker.id);
                level.pop_front();
            }
        }
        if (level.empty()) {
            opposite.erase(level_it);
        }
    }
}

void OrderBook::rest(OrderId id, Side side, Price price, Quantity quantity) {
    if (side == Side::Buy) {
        Level &level = bids_[price];
        level.push_back(Order{id, side, price, quantity});
        index_[id] = Locator{side, price, std::prev(level.end())};
    } else {
        Level &level = asks_[price];
        level.push_back(Order{id, side, price, quantity});
        index_[id] = Locator{side, price, std::prev(level.end())};
    }
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                        TimeInForce tif) {
    std::vector<Trade> trades;
    if (side == Side::Buy) {
        match_against(asks_, id, true, price, false, quantity, trades);
    } else {
        match_against(bids_, id, false, price, false, quantity, trades);
    }
    if (quantity > 0 && tif == TimeInForce::GTC) {
        rest(id, side, price, quantity);
    }
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    std::vector<Trade> trades;
    if (side == Side::Buy) {
        match_against(asks_, id, true, 0, true, quantity, trades);
    } else {
        match_against(bids_, id, false, 0, true, quantity, trades);
    }
    return trades; // market orders never rest
}

bool OrderBook::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    const Locator loc = found->second;
    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            asks_.erase(level_it);
        }
    }
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
    const Level *level = nullptr;
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        if (it != bids_.end()) {
            level = &it->second;
        }
    } else {
        const auto it = asks_.find(price);
        if (it != asks_.end()) {
            level = &it->second;
        }
    }
    QuantityTotal total = 0;
    if (level != nullptr) {
        for (const Order &o : *level) {
            total += static_cast<QuantityTotal>(o.quantity);
        }
    }
    return total;
}

std::size_t OrderBook::order_count() const {
    return index_.size();
}

bool OrderBook::contains(OrderId id) const {
    return index_.find(id) != index_.end();
}

} // namespace qsl::engine

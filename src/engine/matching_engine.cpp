#include "qsl/engine/matching_engine.hpp"

#include <string>

namespace qsl::engine {

MatchingEngine::MatchingEngine(OrderBook::Storage storage) : book_storage_(storage) {}

SymbolId SymbolRegistry::intern(std::string_view name) {
    const std::string key(name);
    const auto it = ids_.find(key);
    if (it != ids_.end()) {
        return it->second;
    }
    const SymbolId id = next_++;
    ids_.emplace(key, id);
    return id;
}

std::optional<SymbolId> SymbolRegistry::find(std::string_view name) const {
    const auto it = ids_.find(std::string(name));
    if (it == ids_.end()) {
        return std::nullopt;
    }
    return it->second;
}

SymbolId MatchingEngine::register_symbol(std::string_view name) {
    const SymbolId id = registry_.intern(name);
    books_.try_emplace(id, book_storage_); // constructs OrderBook(book_storage_) in place
    return id;
}

std::optional<SymbolId> MatchingEngine::symbol_id(std::string_view name) const {
    return registry_.find(name);
}

OrderBook *MatchingEngine::find_book(SymbolId symbol) noexcept {
    const auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : &it->second;
}

std::vector<EngineEvent> MatchingEngine::new_limit(SymbolId symbol, OrderId id, Side side,
                                                   Price price, Quantity quantity,
                                                   TimeInForce tif) {
    std::vector<EngineEvent> events;
    OrderBook *book = find_book(symbol);
    if (book == nullptr) {
        return events; // unknown symbol: rejection is the risk layer's job (M5)
    }
    if (book->contains(id)) {
        return events; // duplicate active id: structured rejection is added in M5
    }
    if (!book->can_store_limit(side, price, quantity, tif)) {
        return events;
    }
    events.push_back(OrderAccepted{next_seq(), symbol, id});
    for (const Trade &t : book->add_limit(id, side, price, quantity, tif)) {
        events.push_back(
            TradeEvent{next_seq(), symbol, t.taker_id, t.maker_id, t.price, t.quantity});
    }
    return events;
}

std::vector<EngineEvent> MatchingEngine::new_market(SymbolId symbol, OrderId id, Side side,
                                                    Quantity quantity) {
    std::vector<EngineEvent> events;
    OrderBook *book = find_book(symbol);
    if (book == nullptr) {
        return events;
    }
    if (book->contains(id)) {
        return events; // duplicate active id: structured rejection is added in M5
    }
    events.push_back(OrderAccepted{next_seq(), symbol, id});
    for (const Trade &t : book->add_market(id, side, quantity)) {
        events.push_back(
            TradeEvent{next_seq(), symbol, t.taker_id, t.maker_id, t.price, t.quantity});
    }
    return events;
}

std::vector<EngineEvent> MatchingEngine::cancel(SymbolId symbol, OrderId id) {
    std::vector<EngineEvent> events;
    OrderBook *book = find_book(symbol);
    if (book == nullptr) {
        return events;
    }
    if (book->cancel(id)) {
        events.push_back(OrderCanceled{next_seq(), symbol, id});
    }
    return events;
}

std::vector<EngineEvent> MatchingEngine::modify(SymbolId symbol, OrderId id, Price new_price,
                                                Quantity new_quantity) {
    std::vector<EngineEvent> events;
    OrderBook *book = find_book(symbol);
    if (book == nullptr || !book->contains(id)) {
        return events; // unknown symbol/order: rejection is the risk layer's job (M5)
    }
    events.push_back(OrderModified{next_seq(), symbol, id});
    for (const Trade &t : book->modify(id, new_price, new_quantity)) {
        events.push_back(
            TradeEvent{next_seq(), symbol, t.taker_id, t.maker_id, t.price, t.quantity});
    }
    return events;
}

bool MatchingEngine::has_symbol(SymbolId symbol) const {
    return books_.find(symbol) != books_.end();
}

bool MatchingEngine::contains(SymbolId symbol, OrderId id) const {
    const auto it = books_.find(symbol);
    return it != books_.end() && it->second.contains(id);
}

std::optional<Price> MatchingEngine::best_bid(SymbolId symbol) const {
    const auto it = books_.find(symbol);
    return it == books_.end() ? std::nullopt : it->second.best_bid();
}

std::optional<Price> MatchingEngine::best_ask(SymbolId symbol) const {
    const auto it = books_.find(symbol);
    return it == books_.end() ? std::nullopt : it->second.best_ask();
}

std::size_t MatchingEngine::fill_count(SymbolId symbol, Side side, Price price, OrderType type,
                                       Quantity quantity) const {
    const auto it = books_.find(symbol);
    if (it == books_.end()) {
        return 0;
    }
    return it->second.fill_count(side, price, type == OrderType::Market, quantity);
}

bool MatchingEngine::can_store_limit(SymbolId symbol, Side side, Price price, Quantity quantity,
                                     TimeInForce tif) const {
    const auto it = books_.find(symbol);
    return it != books_.end() && it->second.can_store_limit(side, price, quantity, tif);
}

EngineSnapshot MatchingEngine::snapshot() const {
    EngineSnapshot snap;
    snap.last_seq = seq_;
    for (const auto &[id, book] : books_) {
        snap.symbols.push_back(SymbolSnapshot{id, book.best_bid(), book.best_ask(),
                                              book.order_count(), book.bid_levels(),
                                              book.ask_levels()});
    }
    return snap;
}

} // namespace qsl::engine

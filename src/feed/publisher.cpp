#include "qsl/feed/publisher.hpp"

#include <algorithm>
#include <variant>

namespace qsl::feed {

void MarketDataPublisher::subscribe(MarketDataSubscriber &subscriber) {
    subscribers_.push_back(&subscriber);
}

void MarketDataPublisher::emit(const MarketDataMessage &msg) {
    for (MarketDataSubscriber *s : subscribers_) {
        s->on_market_data(msg);
    }
}

void MarketDataPublisher::publish(const MatchingEngine &engine,
                                  std::span<const EngineEvent> events) {
    std::vector<SymbolId> touched; // first-appearance order, for deterministic TOB emission
    for (const EngineEvent &ev : events) {
        const SymbolId symbol = engine::symbol_of(ev);
        if (std::holds_alternative<engine::TradeEvent>(ev)) {
            const auto &t = std::get<engine::TradeEvent>(ev);
            emit(MdTrade{++md_seq_, symbol, t.price, t.quantity});
        }
        if (std::find(touched.begin(), touched.end(), symbol) == touched.end()) {
            touched.push_back(symbol);
        }
    }
    for (const SymbolId symbol : touched) {
        const Tob current{engine.best_bid(symbol), engine.best_ask(symbol)};
        const auto it = last_tob_.find(symbol);
        if (it == last_tob_.end()) {
            last_tob_[symbol] = current;
            if (current.bid.has_value() || current.ask.has_value()) {
                emit(MdTopOfBook{++md_seq_, symbol, current.bid, current.ask});
            }
        } else if (it->second != current) {
            emit(MdTopOfBook{++md_seq_, symbol, current.bid, current.ask});
            it->second = current;
        }
    }
}

} // namespace qsl::feed

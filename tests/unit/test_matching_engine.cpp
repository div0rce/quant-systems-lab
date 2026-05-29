#include "qsl/engine/matching_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <variant>
#include <vector>

using namespace qsl::engine;

namespace {
void append(std::vector<EngineEvent> &all, std::vector<EngineEvent> evs) {
    for (auto &e : evs) {
        all.push_back(e);
    }
}
} // namespace

TEST_CASE("symbol registry assigns stable, distinct ids", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    REQUIRE(eng.register_symbol("AAPL") == a); // idempotent
    const SymbolId m = eng.register_symbol("MSFT");
    REQUIRE(m != a);
    REQUIRE(eng.symbol_id("AAPL") == std::optional<SymbolId>{a});
    REQUIRE(eng.symbol_id("MSFT") == std::optional<SymbolId>{m});
    REQUIRE_FALSE(eng.symbol_id("NVDA").has_value());
}

TEST_CASE("multiple symbols trade independently", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    const SymbolId m = eng.register_symbol("MSFT");

    eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto ev = eng.new_limit(a, 2, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(ev.size() == 2);
    REQUIRE(std::holds_alternative<OrderAccepted>(ev[0]));
    REQUIRE(std::holds_alternative<TradeEvent>(ev[1]));
    const auto &tr = std::get<TradeEvent>(ev[1]);
    REQUIRE(tr.symbol == a);
    REQUIRE(tr.maker_id == 1);
    REQUIRE(tr.taker_id == 2);
    REQUIRE(tr.price == 100);
    REQUIRE(tr.quantity == 5);

    eng.new_limit(m, 3, Side::Sell, 101, 3, TimeInForce::GTC); // rests on MSFT only

    const auto snap = eng.snapshot();
    REQUIRE(snap.symbols.size() == 2);
    REQUIRE(snap.symbols[0].symbol == a); // ordered by SymbolId
    REQUIRE(snap.symbols[0].order_count == 0);
    REQUIRE_FALSE(snap.symbols[0].best_ask.has_value());
    REQUIRE(snap.symbols[1].symbol == m);
    REQUIRE(snap.symbols[1].order_count == 1);
    REQUIRE(snap.symbols[1].best_ask == std::optional<Price>{101});
}

TEST_CASE("sequence numbers strictly increase across commands", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");

    std::vector<EngineEvent> all;
    append(all, eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC));
    append(all, eng.new_limit(a, 2, Side::Sell, 101, 5, TimeInForce::GTC));
    append(all, eng.new_market(a, 3, Side::Buy, 7)); // sweeps 100 then 101
    append(all, eng.cancel(a, 2));                   // cancels the remaining ask

    REQUIRE(all.size() == 6);
    SeqNo prev = 0;
    for (const auto &ev : all) {
        REQUIRE(seq_of(ev) > prev);
        prev = seq_of(ev);
    }
    REQUIRE(eng.last_seq() == prev);
}

TEST_CASE("same commands produce identical events and snapshot", "[engine]") {
    const auto scenario = [](MatchingEngine &e) {
        std::vector<EngineEvent> all;
        const SymbolId a = e.register_symbol("AAPL");
        const SymbolId m = e.register_symbol("MSFT");
        append(all, e.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC));
        append(all, e.new_limit(m, 2, Side::Sell, 200, 4, TimeInForce::GTC));
        append(all, e.new_limit(a, 3, Side::Sell, 100, 2, TimeInForce::GTC)); // crosses AAPL bid
        append(all, e.modify(m, 2, 201, 4));                                  // reprice MSFT ask
        return all;
    };

    MatchingEngine e1;
    MatchingEngine e2;
    REQUIRE(scenario(e1) == scenario(e2));
    REQUIRE(e1.snapshot() == e2.snapshot());
}

TEST_CASE("engine routes cancel and emits events", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);

    const auto canceled = eng.cancel(a, 1);
    REQUIRE(canceled.size() == 1);
    REQUIRE(std::holds_alternative<OrderCanceled>(canceled[0]));
    REQUIRE(eng.cancel(a, 1).empty());   // already gone
    REQUIRE(eng.cancel(a, 999).empty()); // unknown order
}

TEST_CASE("modify emits OrderModified and any resulting trades", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");

    eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(eng.modify(a, 1, 100, 3).size() == 1); // in-place reduce -> just Modified
    REQUIRE(eng.modify(a, 999, 100, 3).empty());   // unknown order -> no-op

    eng.new_limit(a, 2, Side::Sell, 101, 5, TimeInForce::GTC);
    const auto ev = eng.modify(a, 2, 100, 5); // ask repriced to cross the bid
    REQUIRE(ev.size() == 2);
    REQUIRE(std::holds_alternative<OrderModified>(ev[0]));
    REQUIRE(std::holds_alternative<TradeEvent>(ev[1]));
    REQUIRE(std::get<TradeEvent>(ev[1]).maker_id == 1);
}

TEST_CASE("command for an unregistered symbol is a no-op", "[engine]") {
    MatchingEngine eng;
    const auto ev = eng.new_limit(SymbolId{42}, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(ev.empty());
    REQUIRE(eng.last_seq() == 0);
    REQUIRE(eng.snapshot().symbols.empty());
}

TEST_CASE("duplicate active limit id emits no events and consumes no sequence", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");

    const auto accepted = eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(accepted.size() == 1);
    REQUIRE(eng.last_seq() == 1);

    const auto duplicate = eng.new_limit(a, 1, Side::Buy, 99, 7, TimeInForce::GTC);
    REQUIRE(duplicate.empty());
    REQUIRE(eng.last_seq() == 1);

    const auto sell = eng.new_limit(a, 2, Side::Sell, 100, 5, TimeInForce::GTC);
    REQUIRE(sell.size() == 2);
    REQUIRE(seq_of(sell[0]) == 2);
    REQUIRE(seq_of(sell[1]) == 3);
}

TEST_CASE("duplicate active market id emits no events and consumes no sequence", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");

    const auto accepted = eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    REQUIRE(accepted.size() == 1);
    REQUIRE(eng.last_seq() == 1);

    const auto duplicate = eng.new_market(a, 1, Side::Buy, 5);
    REQUIRE(duplicate.empty());
    REQUIRE(eng.last_seq() == 1);

    const auto market = eng.new_market(a, 2, Side::Buy, 5);
    REQUIRE(market.size() == 2);
    REQUIRE(seq_of(market[0]) == 2);
    REQUIRE(seq_of(market[1]) == 3);
}

TEST_CASE("sequence numbers are global across symbols", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    const SymbolId m = eng.register_symbol("MSFT");

    std::vector<EngineEvent> all;
    append(all, eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC)); // AAPL accepted
    append(all, eng.new_limit(m, 2, Side::Sell, 200, 5, TimeInForce::GTC)); // MSFT accepted
    append(all, eng.new_limit(a, 3, Side::Buy, 100, 5, TimeInForce::GTC));  // AAPL accepted + trade

    REQUIRE(all.size() == 4);
    SeqNo prev = 0;
    for (const auto &ev : all) {
        REQUIRE(seq_of(ev) > prev); // strictly increases across symbols
        prev = seq_of(ev);
    }
    // One global counter: the MSFT event sits between the AAPL events, not on its own series.
    REQUIRE(seq_of(all[1]) > seq_of(all[0])); // MSFT accepted > first AAPL accepted
    REQUIRE(seq_of(all[2]) > seq_of(all[1])); // later AAPL accepted > MSFT accepted
}

TEST_CASE("market order emits OrderAccepted then a TradeEvent", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC); // resting maker

    const auto ev = eng.new_market(a, 2, Side::Buy, 5);
    REQUIRE(ev.size() == 2);
    REQUIRE(std::holds_alternative<OrderAccepted>(ev[0]));
    REQUIRE(std::holds_alternative<TradeEvent>(ev[1]));

    const auto &acc = std::get<OrderAccepted>(ev[0]);
    REQUIRE(acc.symbol == a);
    REQUIRE(acc.order_id == 2);

    const auto &tr = std::get<TradeEvent>(ev[1]);
    REQUIRE(tr.symbol == a);
    REQUIRE(tr.taker_id == 2);
    REQUIRE(tr.maker_id == 1);
    REQUIRE(tr.price == 100); // resting maker price
    REQUIRE(tr.quantity == 5);
    REQUIRE(seq_of(ev[1]) > seq_of(ev[0]));
}

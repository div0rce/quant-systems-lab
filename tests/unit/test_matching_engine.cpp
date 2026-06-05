#include "qsl/engine/matching_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
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

// A TradeEvent with the given maker/taker/price/quantity on `symbol`.
struct ExpectedTrade {
    SymbolId symbol;
    OrderId maker;
    OrderId taker;
    Price price;
    Quantity quantity;
};

void expect_trade(const EngineEvent &ev, const ExpectedTrade &expected) {
    REQUIRE(std::holds_alternative<TradeEvent>(ev));
    const auto &tr = std::get<TradeEvent>(ev);
    CAPTURE(expected.symbol, expected.maker, expected.taker, expected.price, expected.quantity);
    REQUIRE(tr.symbol == expected.symbol);
    REQUIRE(tr.maker_id == expected.maker);
    REQUIRE(tr.taker_id == expected.taker);
    REQUIRE(tr.price == expected.price);
    REQUIRE(tr.quantity == expected.quantity);
}

// An OrderAccepted for `order_id` on `symbol`.
void expect_accepted(const EngineEvent &ev, SymbolId symbol, OrderId order_id) {
    REQUIRE(std::holds_alternative<OrderAccepted>(ev));
    const auto &acc = std::get<OrderAccepted>(ev);
    REQUIRE(acc.symbol == symbol);
    REQUIRE(acc.order_id == order_id);
}

// A per-symbol snapshot view: registered `symbol`, `order_count` resting orders, and `best_ask`.
void expect_symbol(const auto &view, SymbolId symbol, std::size_t order_count,
                   std::optional<Price> best_ask) {
    CAPTURE(symbol);
    REQUIRE(view.symbol == symbol);
    REQUIRE(view.order_count == order_count);
    REQUIRE(view.best_ask == best_ask);
}

// A duplicate active order id is a no-op: it emits nothing and consumes no sequence number.
void expect_duplicate_noop(const std::vector<EngineEvent> &dup_events, const MatchingEngine &eng,
                           SeqNo seq_before) {
    REQUIRE(dup_events.empty());
    REQUIRE(eng.last_seq() == seq_before);
}

// Assert every event's sequence number strictly increases; returns the final (highest) one.
SeqNo expect_strictly_increasing(const std::vector<EngineEvent> &events) {
    SeqNo prev = 0;
    for (const auto &ev : events) {
        REQUIRE(seq_of(ev) > prev);
        prev = seq_of(ev);
    }
    return prev;
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
    expect_trade(ev[1], ExpectedTrade{a, /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/5});

    eng.new_limit(m, 3, Side::Sell, 101, 3, TimeInForce::GTC); // rests on MSFT only

    const auto snap = eng.snapshot();
    REQUIRE(snap.symbols.size() == 2);
    expect_symbol(snap.symbols[0], a, /*order_count=*/0,
                  /*best_ask=*/std::nullopt); // ordered by id
    expect_symbol(snap.symbols[1], m, /*order_count=*/1, std::optional<Price>{101});
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
    REQUIRE(eng.last_seq() == expect_strictly_increasing(all));
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
    // order 1 (buy@100) was reduced to qty 3 above, so the cross executes qty 3, not 5
    expect_trade(ev[1], ExpectedTrade{a, /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/3});
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

    REQUIRE(eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC).size() == 1);
    REQUIRE(eng.last_seq() == 1);

    expect_duplicate_noop(eng.new_limit(a, 1, Side::Buy, 99, 7, TimeInForce::GTC), eng, 1);

    const auto sell = eng.new_limit(a, 2, Side::Sell, 100, 5, TimeInForce::GTC);
    REQUIRE(sell.size() == 2);
    REQUIRE(seq_of(sell[0]) == 2);
    REQUIRE(seq_of(sell[1]) == 3);
}

TEST_CASE("duplicate active market id emits no events and consumes no sequence", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");

    REQUIRE(eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC).size() == 1);
    REQUIRE(eng.last_seq() == 1);

    expect_duplicate_noop(eng.new_market(a, 1, Side::Buy, 5), eng, 1);

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
    expect_strictly_increasing(all);
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
    expect_accepted(ev[0], a, /*order_id=*/2);
    expect_trade(ev[1], ExpectedTrade{a, /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/5});
    REQUIRE(seq_of(ev[1]) > seq_of(ev[0]));
}

#include "qsl/engine/matching_engine.hpp"
#include "qsl/replay/recovery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
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
    const auto actual_fields =
        std::tuple{tr.symbol, tr.maker_id, tr.taker_id, tr.price, tr.quantity};
    const auto expected_fields = std::tuple{expected.symbol, expected.maker, expected.taker,
                                            expected.price, expected.quantity};
    REQUIRE(actual_fields == expected_fields);
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

OrderId fill_intrusive_pool_with_one_crossable_maker(MatchingEngine &eng, SymbolId symbol) {
    REQUIRE(eng.new_limit(symbol, 1, Side::Sell, 100, 1, TimeInForce::GTC).size() == 1);

    OrderId next_id = 2;
    for (; next_id < 100'000; ++next_id) {
        const auto ev = eng.new_limit(symbol, next_id, Side::Sell, 101, 1, TimeInForce::GTC);
        if (ev.empty()) {
            break;
        }
        REQUIRE(ev.size() == 1);
    }
    REQUIRE(next_id < 100'000); // the intrusive pool is full
    return next_id;
}

void expect_residual_bid_after_capacity_free(const MatchingEngine &eng, SymbolId symbol,
                                             OrderId taker_id) {
    REQUIRE(eng.contains(symbol, taker_id));
    const auto snap = eng.snapshot();
    REQUIRE(snap.symbols.size() == 1);
    const auto actual = std::tuple{snap.symbols[0].best_bid, snap.symbols[0].best_ask};
    const auto expected = std::tuple{std::optional<Price>{100}, std::optional<Price>{101}};
    REQUIRE(actual == expected);
}
} // namespace

TEST_CASE("symbol registry assigns stable, distinct ids", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    REQUIRE(eng.register_symbol("AAPL") == a); // idempotent
    const SymbolId m = eng.register_symbol("MSFT");
    REQUIRE(m != a);
    const bool lookups_match = eng.symbol_id("AAPL") == std::optional<SymbolId>{a} &&
                               eng.symbol_id("MSFT") == std::optional<SymbolId>{m} &&
                               !eng.symbol_id("NVDA").has_value();
    REQUIRE(lookups_match);
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

TEST_CASE("storage modes produce identical events and final snapshot", "[engine][storage]") {
    const auto flow = qsl::replay::generate_flow(/*seed=*/91, /*symbols=*/4, /*orders=*/800);
    const auto run = [&](OrderBook::Storage storage) {
        MatchingEngine eng{storage};
        std::vector<EngineEvent> events;
        for (const auto &command : flow) {
            append(events, qsl::replay::apply(eng, command));
        }
        return std::pair{events, eng.snapshot()};
    };

    const auto baseline = run(OrderBook::Storage::Baseline);
    const auto pmr = run(OrderBook::Storage::Pooled);
    const auto intrusive = run(OrderBook::Storage::IntrusivePooled);

    REQUIRE(pmr == baseline);
    REQUIRE(intrusive == baseline);
    const bool non_vacuous = baseline.second.last_seq > 0 && !baseline.second.symbols.empty();
    REQUIRE(non_vacuous);
}

TEST_CASE("intrusive storage rests only the post-match remainder", "[engine][storage]") {
    // Regression: a partially filled GTC limit must rest its leftover quantity, not the original
    // input, and a fully filled one must rest nothing. A 4-lot maker against a 10-lot taker leaves
    // a 6-lot resting taker (not 10); a later 6-lot opposite order then clears the book entirely.
    const auto run = [](OrderBook::Storage storage) {
        MatchingEngine eng{storage};
        const SymbolId a = eng.register_symbol("AAPL");
        eng.new_limit(a, 1, Side::Sell, 100, 4, TimeInForce::GTC); // maker: 4 lots
        eng.new_limit(a, 2, Side::Buy, 100, 10, TimeInForce::GTC); // fills 4, must rest 6 (not 10)
        eng.new_limit(a, 3, Side::Sell, 100, 6, TimeInForce::GTC); // fully clears the resting 6
        return eng.snapshot();
    };

    const auto baseline = run(OrderBook::Storage::Baseline);
    const auto intrusive = run(OrderBook::Storage::IntrusivePooled);
    REQUIRE(intrusive == baseline);
    // Resting 10 (the bug) would leave a 4-lot bid after order 3 and a wrongly-rested ask; the
    // correct remainder of 6 is fully traded, so the book ends empty.
    REQUIRE(intrusive.symbols.size() == 1);
    REQUIRE(intrusive.symbols[0].order_count == 0);
}

TEST_CASE("intrusive storage accepts residual when a match frees capacity", "[engine][storage]") {
    MatchingEngine eng{OrderBook::Storage::IntrusivePooled};
    const SymbolId a = eng.register_symbol("AAPL");

    const OrderId taker_id = fill_intrusive_pool_with_one_crossable_maker(eng, a) + 1;
    const auto ev = eng.new_limit(a, taker_id, Side::Buy, 100, 2, TimeInForce::GTC);
    REQUIRE(ev.size() == 2);
    expect_accepted(ev[0], a, taker_id);
    expect_trade(ev[1], ExpectedTrade{a, /*maker=*/1, /*taker=*/taker_id, /*price=*/100,
                                      /*quantity=*/1});
    expect_residual_bid_after_capacity_free(eng, a, taker_id);
}

TEST_CASE("engine routes cancel and emits events", "[engine]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);

    const auto canceled = eng.cancel(a, 1);
    REQUIRE(canceled.size() == 1);
    REQUIRE(std::holds_alternative<OrderCanceled>(canceled[0]));
    const bool later_cancels_noop = eng.cancel(a, 1).empty() && eng.cancel(a, 999).empty();
    REQUIRE(later_cancels_noop);
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

TEST_CASE("book state rebuilt from resting_orders matches the original", "[engine][resting]") {
    // The capture/restore procedure the M46 recovery benchmark times: enumerate every resting
    // order in priority order, then re-add the orders into a fresh engine registered with the
    // same symbol names. Book state (levels, tops, counts) and intra-level FIFO order must be
    // reproduced exactly; only the sequence counter differs (the rebuilt engine re-counts
    // accepts), which is why a real snapshot design would also have to persist sequencing state.
    const auto flow = qsl::replay::generate_flow(/*seed=*/7, /*symbols=*/3, /*orders=*/600);
    MatchingEngine original;
    std::vector<std::string> names;
    for (const auto &command : flow) {
        if (const auto *reg = std::get_if<qsl::replay::RegisterSymbol>(&command)) {
            names.push_back(reg->name);
        }
        static_cast<void>(qsl::replay::apply(original, command));
    }

    MatchingEngine rebuilt;
    std::size_t total_resting = 0;
    for (const auto &name : names) {
        const SymbolId symbol = rebuilt.register_symbol(name);
        for (const Order &order : original.resting_orders(symbol)) {
            const auto ev = rebuilt.new_limit(symbol, order.id, order.side, order.price,
                                              order.quantity, TimeInForce::GTC);
            REQUIRE(ev.size() == 1); // accepted only: re-adding uncrossed state never trades
            ++total_resting;
        }
    }

    REQUIRE(total_resting > 10); // non-vacuity: the flow must leave a real book behind
    REQUIRE(rebuilt.snapshot().symbols == original.snapshot().symbols);
    for (SymbolId symbol = 0; symbol < 3; ++symbol) {
        CAPTURE(symbol);
        REQUIRE(rebuilt.resting_orders(symbol) == original.resting_orders(symbol));
    }
}

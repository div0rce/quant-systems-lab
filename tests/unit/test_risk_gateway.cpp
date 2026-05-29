#include "qsl/gateway/order_gateway.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <variant>

using namespace qsl::gateway;
namespace engine = qsl::engine;

namespace {
constexpr RiskConfig kConfig{/*max_order_quantity=*/1000, /*max_notional=*/1'000'000};
} // namespace

TEST_CASE("accepted limit forwards to the engine", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    const auto r = gw.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(r.accepted);
    REQUIRE(r.reason == RejectReason::None);
    REQUIRE(r.events.size() == 1);
    REQUIRE(std::holds_alternative<engine::OrderAccepted>(r.events[0]));
    REQUIRE(eng.last_seq() == 1);
    REQUIRE(eng.snapshot().symbols[0].best_bid == std::optional<Price>{100});
}

TEST_CASE("accepted crossing limit forwards trades", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    gw.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto r = gw.new_limit(a, 2, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 2);
    REQUIRE(std::holds_alternative<engine::TradeEvent>(r.events[1]));
}

TEST_CASE("accepted market order forwards", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    gw.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto r = gw.new_market(a, 2, Side::Buy, 5);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 2); // OrderAccepted + TradeEvent
}

TEST_CASE("unknown symbol rejects and does not reach the engine", "[gateway]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    const auto r = gw.new_limit(SymbolId{99}, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.reason == RejectReason::UnknownSymbol);
    REQUIRE(r.events.empty());
    REQUIRE(eng.last_seq() == 0);
}

TEST_CASE("duplicate active order id rejects", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    REQUIRE(gw.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted); // rests
    const auto dup = gw.new_limit(a, 1, Side::Buy, 101, 5, TimeInForce::GTC);
    REQUIRE_FALSE(dup.accepted);
    REQUIRE(dup.reason == RejectReason::DuplicateOrderId);
    REQUIRE(eng.last_seq() == 1); // dup did not reach the engine
}

TEST_CASE("invalid side rejects", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    const auto r = gw.new_limit(a, 1, static_cast<Side>(9), 100, 5, TimeInForce::GTC);
    REQUIRE(r.reason == RejectReason::InvalidSide);
}

TEST_CASE("invalid price rejects without reaching the engine", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    const auto before = eng.snapshot();
    const auto r = gw.new_limit(a, 1, Side::Buy, 0, 5, TimeInForce::GTC);
    REQUIRE(r.reason == RejectReason::InvalidPrice);
    REQUIRE(eng.last_seq() == 0);
    REQUIRE(eng.snapshot() == before);
}

TEST_CASE("invalid quantity rejects", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    REQUIRE(gw.new_limit(a, 1, Side::Buy, 100, 0, TimeInForce::GTC).reason ==
            RejectReason::InvalidQuantity);
    REQUIRE(gw.new_market(a, 2, Side::Buy, 0).reason == RejectReason::InvalidQuantity);
}

TEST_CASE("max quantity exceeded rejects", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    const auto r = gw.new_limit(a, 1, Side::Buy, 100, 2000, TimeInForce::GTC);
    REQUIRE(r.reason == RejectReason::MaxQuantityExceeded);
}

TEST_CASE("max notional exceeded rejects", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, RiskConfig{/*max_order_quantity=*/1000, /*max_notional=*/10'000});

    // quantity 200 <= max_order_quantity, but 100 * 200 = 20000 > 10000
    const auto r = gw.new_limit(a, 1, Side::Buy, 100, 200, TimeInForce::GTC);
    REQUIRE(r.reason == RejectReason::MaxNotionalExceeded);
}

TEST_CASE("cancel unknown order rejects; cancel resting order forwards", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    REQUIRE(gw.cancel(a, 999).reason == RejectReason::UnknownOrder);

    gw.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    const auto r = gw.cancel(a, 1);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 1);
    REQUIRE(std::holds_alternative<engine::OrderCanceled>(r.events[0]));
    REQUIRE_FALSE(eng.contains(a, 1));
}

TEST_CASE("modify unknown order rejects; modify resting order forwards", "[gateway]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    REQUIRE(gw.modify(a, 999, 100, 3).reason == RejectReason::UnknownOrder);

    gw.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    const auto r = gw.modify(a, 1, 100, 3);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 1);
    REQUIRE(std::holds_alternative<engine::OrderModified>(r.events[0]));
}

TEST_CASE("cancel/modify on an unknown symbol reject", "[gateway]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw(eng, kConfig);

    REQUIRE(gw.cancel(SymbolId{99}, 1).reason == RejectReason::UnknownSymbol);
    REQUIRE(gw.modify(SymbolId{99}, 1, 100, 3).reason == RejectReason::UnknownSymbol);
}

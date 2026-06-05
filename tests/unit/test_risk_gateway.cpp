#include "qsl/gateway/order_gateway.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <variant>

using namespace qsl::gateway;
namespace engine = qsl::engine;

namespace {
constexpr RiskConfig kConfig{/*max_order_quantity=*/1000, /*max_notional=*/1'000'000};

// One engine + gateway over a single registered symbol "AAPL" (id `a`) -- the setup every case
// shares.
struct Gateway {
    explicit Gateway(RiskConfig config = kConfig) : gw(eng, config) {}
    MatchingEngine eng;
    SymbolId a = eng.register_symbol("AAPL");
    OrderGateway gw;
};

// A rejected command: not accepted, carries the expected reason, and emits no engine events.
void expect_reject(const GatewayResult &r, RejectReason reason) {
    CAPTURE(reason);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.reason == reason);
    REQUIRE(r.events.empty());
}

// A rejection that left engine state untouched: sequence and snapshot identical to `before`.
void expect_unchanged(const MatchingEngine &eng, const auto &before, std::uint64_t before_seq) {
    REQUIRE(eng.last_seq() == before_seq);
    REQUIRE(eng.snapshot() == before);
}

// An accepted command that emitted exactly one engine event of variant type E.
template <class E> void expect_accepted_one(const GatewayResult &r) {
    REQUIRE(r.accepted);
    REQUIRE(r.reason == RejectReason::None);
    REQUIRE(r.events.size() == 1);
    REQUIRE(std::holds_alternative<E>(r.events[0]));
}
} // namespace

TEST_CASE("accepted limit forwards to the engine", "[gateway]") {
    Gateway fx;
    const auto r = fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    expect_accepted_one<engine::OrderAccepted>(r);
    REQUIRE(fx.eng.last_seq() == 1);
    REQUIRE(fx.eng.snapshot().symbols[0].best_bid == std::optional<Price>{100});
}

TEST_CASE("accepted crossing limit forwards trades", "[gateway]") {
    Gateway fx;
    fx.gw.new_limit(fx.a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto r = fx.gw.new_limit(fx.a, 2, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 2);
    REQUIRE(std::holds_alternative<engine::TradeEvent>(r.events[1]));
}

TEST_CASE("accepted market order forwards", "[gateway]") {
    Gateway fx;
    fx.gw.new_limit(fx.a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto r = fx.gw.new_market(fx.a, 2, Side::Buy, 5);
    REQUIRE(r.accepted);
    REQUIRE(r.events.size() == 2); // OrderAccepted + TradeEvent
}

TEST_CASE("market order unknown symbol rejects and does not reach the engine", "[gateway]") {
    Gateway fx;
    const auto before = fx.eng.snapshot();
    const auto r = fx.gw.new_market(SymbolId{99}, 1, Side::Buy, 5);
    expect_reject(r, RejectReason::UnknownSymbol);
    expect_unchanged(fx.eng, before, 0);
}

TEST_CASE("unknown symbol rejects and does not reach the engine", "[gateway]") {
    Gateway fx;
    const auto r = fx.gw.new_limit(SymbolId{99}, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    expect_reject(r, RejectReason::UnknownSymbol);
    REQUIRE(fx.eng.last_seq() == 0);
}

TEST_CASE("duplicate active order id rejects", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted); // rests
    const auto dup = fx.gw.new_limit(fx.a, 1, Side::Buy, 101, 5, TimeInForce::GTC);
    expect_reject(dup, RejectReason::DuplicateOrderId);
    REQUIRE(fx.eng.last_seq() == 1); // dup did not reach the engine
}

TEST_CASE("duplicate active market order id rejects without consuming sequence", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Sell, 100, 5, TimeInForce::GTC).accepted); // rests
    const auto before = fx.eng.snapshot();
    const auto dup = fx.gw.new_market(fx.a, 1, Side::Buy, 5);
    expect_reject(dup, RejectReason::DuplicateOrderId);
    expect_unchanged(fx.eng, before, 1);
}

TEST_CASE("invalid side rejects", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.new_limit(fx.a, 1, static_cast<Side>(9), 100, 5, TimeInForce::GTC),
                  RejectReason::InvalidSide);
}

TEST_CASE("invalid market side rejects", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.new_market(fx.a, 1, static_cast<Side>(9), 5), RejectReason::InvalidSide);
    REQUIRE(fx.eng.last_seq() == 0);
}

TEST_CASE("invalid price rejects without reaching the engine", "[gateway]") {
    Gateway fx;
    const auto before = fx.eng.snapshot();
    expect_reject(fx.gw.new_limit(fx.a, 1, Side::Buy, 0, 5, TimeInForce::GTC),
                  RejectReason::InvalidPrice);
    expect_unchanged(fx.eng, before, 0);
}

TEST_CASE("invalid quantity rejects", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 0, TimeInForce::GTC),
                  RejectReason::InvalidQuantity);
    expect_reject(fx.gw.new_market(fx.a, 2, Side::Buy, 0), RejectReason::InvalidQuantity);
}

TEST_CASE("max quantity exceeded rejects", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 2000, TimeInForce::GTC),
                  RejectReason::MaxQuantityExceeded);
}

TEST_CASE("market order max quantity exceeded rejects without reaching the engine", "[gateway]") {
    Gateway fx;
    const auto before = fx.eng.snapshot();
    expect_reject(fx.gw.new_market(fx.a, 1, Side::Buy, 2000), RejectReason::MaxQuantityExceeded);
    expect_unchanged(fx.eng, before, 0);
}

TEST_CASE("max notional exceeded rejects", "[gateway]") {
    Gateway fx{RiskConfig{/*max_order_quantity=*/1000, /*max_notional=*/10'000}};
    // quantity 200 <= max_order_quantity, but 100 * 200 = 20000 > 10000
    expect_reject(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 200, TimeInForce::GTC),
                  RejectReason::MaxNotionalExceeded);
}

TEST_CASE("cancel unknown order rejects; cancel resting order forwards", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.cancel(fx.a, 999), RejectReason::UnknownOrder);

    fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    expect_accepted_one<engine::OrderCanceled>(fx.gw.cancel(fx.a, 1));
    REQUIRE_FALSE(fx.eng.contains(fx.a, 1));
}

TEST_CASE("modify unknown order rejects; modify resting order forwards", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.modify(fx.a, 999, 100, 3), RejectReason::UnknownOrder);

    fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    expect_accepted_one<engine::OrderModified>(fx.gw.modify(fx.a, 1, 100, 3));
}

TEST_CASE("modify rejects invalid price without mutating engine", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted);
    const auto before = fx.eng.snapshot();
    const auto before_seq = fx.eng.last_seq();

    expect_reject(fx.gw.modify(fx.a, 1, 0, 5), RejectReason::InvalidPrice);
    expect_unchanged(fx.eng, before, before_seq);
}

TEST_CASE("modify rejects max quantity without mutating engine", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted);
    const auto before = fx.eng.snapshot();
    const auto before_seq = fx.eng.last_seq();

    expect_reject(fx.gw.modify(fx.a, 1, 100, 2000), RejectReason::MaxQuantityExceeded);
    expect_unchanged(fx.eng, before, before_seq);
}

TEST_CASE("modify rejects max notional without mutating engine", "[gateway]") {
    Gateway fx{RiskConfig{/*max_order_quantity=*/1000, /*max_notional=*/10'000}};
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted);
    const auto before = fx.eng.snapshot();
    const auto before_seq = fx.eng.last_seq();

    expect_reject(fx.gw.modify(fx.a, 1, 100, 200), RejectReason::MaxNotionalExceeded);
    expect_unchanged(fx.eng, before, before_seq);
}

TEST_CASE("modify quantity zero remains cancel via modify", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted);
    expect_accepted_one<engine::OrderModified>(fx.gw.modify(fx.a, 1, 0, 0));
    REQUIRE_FALSE(fx.eng.contains(fx.a, 1));
    expect_reject(fx.gw.cancel(fx.a, 1), RejectReason::UnknownOrder);
}

TEST_CASE("positive nonzero modify within risk limits forwards", "[gateway]") {
    Gateway fx;
    REQUIRE(fx.gw.new_limit(fx.a, 1, Side::Buy, 100, 5, TimeInForce::GTC).accepted);
    const auto before_seq = fx.eng.last_seq();

    expect_accepted_one<engine::OrderModified>(fx.gw.modify(fx.a, 1, 101, 6));
    REQUIRE(fx.eng.last_seq() == before_seq + 1);
    REQUIRE(fx.eng.contains(fx.a, 1));
    REQUIRE(fx.eng.snapshot().symbols[0].best_bid == std::optional<Price>{101});
}

TEST_CASE("cancel/modify on an unknown symbol reject", "[gateway]") {
    Gateway fx;
    expect_reject(fx.gw.cancel(SymbolId{99}, 1), RejectReason::UnknownSymbol);
    expect_reject(fx.gw.modify(SymbolId{99}, 1, 100, 3), RejectReason::UnknownSymbol);
}

#include "qsl/feed/market_data.hpp"
#include "qsl/feed/publisher.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <variant>
#include <vector>

using namespace qsl::feed;
using namespace qsl::engine;

namespace {
struct RecordingSubscriber : MarketDataSubscriber {
    std::vector<MarketDataMessage> msgs;
    void on_market_data(const MarketDataMessage &msg) override { msgs.push_back(msg); }
};
} // namespace

TEST_CASE("a fill produces an MdTrade and a top-of-book update", "[feed]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    MarketDataPublisher pub;
    RecordingSubscriber sub;
    pub.subscribe(sub);

    const auto e1 = eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    pub.publish(eng, e1); // ask rests -> top-of-book(ask 100)
    const auto e2 = eng.new_limit(a, 2, Side::Buy, 100, 5, TimeInForce::GTC);
    pub.publish(eng, e2); // trade -> MdTrade; book empties -> top-of-book(empty)

    REQUIRE(sub.msgs.size() == 3);

    const auto &tob0 = std::get<MdTopOfBook>(sub.msgs[0]);
    REQUIRE(tob0.best_ask == std::optional<Price>{100});
    REQUIRE_FALSE(tob0.best_bid.has_value());

    const auto &trade = std::get<MdTrade>(sub.msgs[1]);
    REQUIRE(trade.symbol == a);
    REQUIRE(trade.price == 100);
    REQUIRE(trade.quantity == 5);

    const auto &tob1 = std::get<MdTopOfBook>(sub.msgs[2]);
    REQUIRE_FALSE(tob1.best_bid.has_value());
    REQUIRE_FALSE(tob1.best_ask.has_value());
}

TEST_CASE("md sequence numbers are monotonic and follow engine order", "[feed]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    MarketDataPublisher pub;
    RecordingSubscriber sub;
    pub.subscribe(sub);

    const auto e1 = eng.new_limit(a, 1, Side::Sell, 100, 3, TimeInForce::GTC);
    pub.publish(eng, e1);
    const auto e2 = eng.new_limit(a, 2, Side::Sell, 101, 3, TimeInForce::GTC);
    pub.publish(eng, e2);
    const auto e3 = eng.new_market(a, 3, Side::Buy, 4); // sweeps 100 then partials 101
    pub.publish(eng, e3);

    SeqNo prev = 0;
    for (const auto &m : sub.msgs) {
        REQUIRE(md_seq_of(m) > prev);
        prev = md_seq_of(m);
    }
    REQUIRE(pub.md_seq() == prev);
}

TEST_CASE("top-of-book is emitted only when it changes", "[feed]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    MarketDataPublisher pub;
    RecordingSubscriber sub;
    pub.subscribe(sub);

    const auto e1 = eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
    pub.publish(eng, e1); // best bid 100 -> top-of-book
    REQUIRE(sub.msgs.size() == 1);

    const auto e2 = eng.new_limit(a, 2, Side::Buy, 99, 5, TimeInForce::GTC);
    pub.publish(eng, e2); // rests behind best bid -> top unchanged -> no message
    REQUIRE(sub.msgs.size() == 1);
}

TEST_CASE("symbols publish independently", "[feed]") {
    MatchingEngine eng;
    const SymbolId a = eng.register_symbol("AAPL");
    const SymbolId m = eng.register_symbol("MSFT");
    MarketDataPublisher pub;
    RecordingSubscriber sub;
    pub.subscribe(sub);

    const auto e1 = eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC);
    pub.publish(eng, e1);
    const auto e2 = eng.new_limit(m, 2, Side::Sell, 200, 5, TimeInForce::GTC);
    pub.publish(eng, e2);
    const auto e3 = eng.new_limit(a, 3, Side::Buy, 100, 5, TimeInForce::GTC);
    pub.publish(eng, e3);

    REQUIRE(std::get<MdTopOfBook>(sub.msgs[0]).symbol == a);
    REQUIRE(std::get<MdTopOfBook>(sub.msgs[1]).symbol == m);
    REQUIRE(std::get<MdTopOfBook>(sub.msgs[1]).best_ask == std::optional<Price>{200});
    REQUIRE(std::get<MdTrade>(sub.msgs[2]).symbol == a);
}

TEST_CASE("same flow yields identical market data", "[feed]") {
    const auto run = [](RecordingSubscriber &sub) {
        MatchingEngine eng;
        const SymbolId a = eng.register_symbol("AAPL");
        const SymbolId m = eng.register_symbol("MSFT");
        MarketDataPublisher pub;
        pub.subscribe(sub);
        const auto e1 = eng.new_limit(a, 1, Side::Buy, 100, 5, TimeInForce::GTC);
        pub.publish(eng, e1);
        const auto e2 = eng.new_limit(m, 2, Side::Sell, 200, 4, TimeInForce::GTC);
        pub.publish(eng, e2);
        const auto e3 = eng.new_limit(a, 3, Side::Sell, 100, 2, TimeInForce::GTC);
        pub.publish(eng, e3);
    };
    RecordingSubscriber s1;
    RecordingSubscriber s2;
    run(s1);
    run(s2);
    REQUIRE(s1.msgs == s2.msgs);
}

TEST_CASE("MdTrade round-trips through the binary protocol", "[feed]") {
    const MdTrade in{/*md_seq=*/7, /*symbol=*/2, /*price=*/12345, /*quantity=*/10};
    const auto frame = encode(in);
    const auto out = decode_md_trade(frame);
    REQUIRE(out.ok());
    REQUIRE(out.value == in);
}

TEST_CASE("MdTopOfBook round-trips, including absent sides", "[feed]") {
    const MdTopOfBook both{8, 3, std::optional<Price>{100}, std::optional<Price>{101}};
    const auto f1 = encode(both);
    REQUIRE(decode_md_top_of_book(f1).value == both);

    const MdTopOfBook bid_only{9, 3, std::optional<Price>{100}, std::nullopt};
    const auto f2 = encode(bid_only);
    REQUIRE(decode_md_top_of_book(f2).value == bid_only);

    const MdTopOfBook empty{10, 3, std::nullopt, std::nullopt};
    const auto f3 = encode(empty);
    REQUIRE(decode_md_top_of_book(f3).value == empty);
}

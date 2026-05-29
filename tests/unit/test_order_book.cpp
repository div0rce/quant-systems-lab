#include "qsl/engine/order_book.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>

using namespace qsl::engine;

TEST_CASE("non-crossing limits rest and set top of book", "[book]") {
    OrderBook book;
    REQUIRE(book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC).empty());
    REQUIRE(book.add_limit(2, Side::Sell, 101, 5, TimeInForce::GTC).empty());

    REQUIRE(book.best_bid() == std::optional<Price>{100});
    REQUIRE(book.best_ask() == std::optional<Price>{101});
    REQUIRE(book.order_count() == 2);
}

TEST_CASE("crossing limit trades at the resting maker price", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto trades = book.add_limit(2, Side::Buy, 105, 5, TimeInForce::GTC);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 1);
    REQUIRE(trades[0].taker_id == 2);
    REQUIRE(trades[0].price == 100); // executes at the resting price, not the aggressor's 105
    REQUIRE(trades[0].quantity == 5);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("sell aggressor crosses resting bids", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    const auto trades = book.add_limit(2, Side::Sell, 99, 5, TimeInForce::GTC);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 1);
    REQUIRE(trades[0].price == 100);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("price-time priority: earlier order at a level fills first", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC); // same price, later

    const auto trades = book.add_limit(3, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 1); // earlier order matched first
    REQUIRE(book.quantity_at(Side::Sell, 100) == 5);
    REQUIRE(book.order_count() == 1);
}

TEST_CASE("partial fill leaves the maker remainder resting", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10, TimeInForce::GTC);

    const auto trades = book.add_limit(2, Side::Buy, 100, 4, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 4);
    REQUIRE(book.quantity_at(Side::Sell, 100) == 6); // remainder keeps resting
    REQUIRE(book.order_count() == 1);                // aggressor fully filled, did not rest
    REQUIRE(book.best_ask() == std::optional<Price>{100});
}

TEST_CASE("market order sweeps best levels until filled", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 3, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 101, 3, TimeInForce::GTC);

    const auto trades = book.add_market(3, Side::Buy, 5);
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].price == 100);
    REQUIRE(trades[0].quantity == 3);
    REQUIRE(trades[1].price == 101);
    REQUIRE(trades[1].quantity == 2);
    REQUIRE(book.quantity_at(Side::Sell, 101) == 1);
    REQUIRE(book.order_count() == 1); // market taker never rests
}

TEST_CASE("market order on an empty book yields nothing and does not rest", "[book]") {
    OrderBook book;
    const auto trades = book.add_market(1, Side::Buy, 5);
    REQUIRE(trades.empty());
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("IOC discards the unfilled remainder", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 2, TimeInForce::GTC);

    const auto trades = book.add_limit(2, Side::Buy, 100, 5, TimeInForce::IOC);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 2);
    REQUIRE(book.order_count() == 0); // ask consumed, IOC remainder not rested
    REQUIRE_FALSE(book.best_bid().has_value());
}

TEST_CASE("GTC rests the remainder after a partial cross", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 2, TimeInForce::GTC);

    const auto trades = book.add_limit(2, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    REQUIRE(book.best_bid() == std::optional<Price>{100});
    REQUIRE(book.quantity_at(Side::Buy, 100) == 3); // remainder rests as a bid
    REQUIRE(book.order_count() == 1);
}

TEST_CASE("cancel removes a resting order", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);

    REQUIRE(book.cancel(1));
    REQUIRE_FALSE(book.best_bid().has_value());
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.cancel(1));   // already gone
    REQUIRE_FALSE(book.cancel(999)); // never existed
}

TEST_CASE("duplicate resting limit id is ignored without orphaned liquidity", "[book]") {
    OrderBook book;
    REQUIRE(book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC).empty());
    REQUIRE(book.add_limit(1, Side::Buy, 99, 7, TimeInForce::GTC).empty());

    REQUIRE(book.order_count() == 1);
    REQUIRE(book.quantity_at(Side::Buy, 100) == 5);
    REQUIRE(book.quantity_at(Side::Buy, 99) == 0);

    REQUIRE(book.cancel(1));
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.best_bid().has_value());

    const auto trades = book.add_limit(2, Side::Sell, 100, 5, TimeInForce::IOC);
    REQUIRE(trades.empty());
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("modify quantity reduction preserves time priority", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC);

    REQUIRE(book.modify(1, 100, 3).empty()); // same price, smaller qty -> in place
    const auto trades = book.add_limit(3, Side::Buy, 100, 3, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 1); // still ahead of order 2
    REQUIRE(trades[0].quantity == 3);
}

TEST_CASE("modify quantity increase loses time priority", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC);

    REQUIRE(book.modify(1, 100, 8).empty()); // qty increase -> requeued behind order 2
    const auto trades = book.add_limit(3, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 2); // order 2 now has priority
}

TEST_CASE("modify price change loses priority and can cross", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 101, 5, TimeInForce::GTC);

    const auto trades = book.modify(2, 100, 5); // ask repriced down to 100 -> crosses the bid
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].maker_id == 1);
    REQUIRE(trades[0].price == 100);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("book is never crossed after matching", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 102, 5, TimeInForce::GTC);
    // Aggressive buy below the ask does not cross; it rests as the new best bid.
    book.add_limit(3, Side::Buy, 101, 5, TimeInForce::GTC);

    REQUIRE(book.best_bid() == std::optional<Price>{101});
    REQUIRE(book.best_ask() == std::optional<Price>{102});
    REQUIRE(book.best_bid().value() < book.best_ask().value());
}

TEST_CASE("quantity_at aggregates without wrapping at the per-order width", "[book]") {
    OrderBook book;
    const Quantity big = std::numeric_limits<Quantity>::max();
    book.add_limit(1, Side::Buy, 100, big, TimeInForce::GTC);
    book.add_limit(2, Side::Buy, 100, 1, TimeInForce::GTC);

    REQUIRE(book.quantity_at(Side::Buy, 100) == static_cast<QuantityTotal>(big) + 1);
}

TEST_CASE("modify to zero quantity cancels the order", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);

    REQUIRE(book.modify(1, 100, 0).empty());
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.best_bid().has_value());
}

TEST_CASE("modify of an unknown order is a no-op", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);

    REQUIRE(book.modify(999, 100, 3).empty());
    REQUIRE(book.order_count() == 1);
    REQUIRE(book.best_bid() == std::optional<Price>{100});
    REQUIRE(book.quantity_at(Side::Buy, 100) == 5);
}

TEST_CASE("partially filled maker retains priority over later orders", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC);

    const auto first = book.add_limit(3, Side::Buy, 100, 3, TimeInForce::GTC);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].maker_id == 1); // order 1 partially filled, 2 remaining

    const auto second = book.add_limit(4, Side::Buy, 100, 3, TimeInForce::GTC);
    REQUIRE(second.size() == 2);
    REQUIRE(second[0].maker_id == 1); // remainder of order 1 fills first
    REQUIRE(second[0].quantity == 2);
    REQUIRE(second[1].maker_id == 2); // only then order 2
    REQUIRE(second[1].quantity == 1);
}

#include "qsl/engine/order_book.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <vector>

using namespace qsl::engine;

namespace {
// A trade executed between resting `maker` and aggressing `taker` at `price` for `quantity`.
void expect_trade(const auto &trade, OrderId maker, OrderId taker, Price price, Quantity quantity) {
    CAPTURE(maker, taker, price, quantity);
    REQUIRE(trade.maker_id == maker);
    REQUIRE(trade.taker_id == taker);
    REQUIRE(trade.price == price);
    REQUIRE(trade.quantity == quantity);
}

// Top of book: best bid and best ask (use std::nullopt for an empty side).
void expect_top(const OrderBook &book, std::optional<Price> best_bid,
                std::optional<Price> best_ask) {
    REQUIRE(book.best_bid() == best_bid);
    REQUIRE(book.best_ask() == best_ask);
}

void expect_modify_refused(OrderBook &book, Price new_price) {
    REQUIRE_FALSE(book.can_apply_modify(1, new_price, 5));
    REQUIRE(book.modify(1, new_price, 5).empty());
}

void expect_bid_order_kept(const OrderBook &book, OrderId id, Price price) {
    REQUIRE(book.contains(id));
    REQUIRE(book.best_bid() == std::optional<Price>{price});
}

void expect_ask_order_kept(const OrderBook &book, OrderId id, Price price) {
    REQUIRE(book.contains(id));
    REQUIRE(book.best_ask() == std::optional<Price>{price});
}
} // namespace

TEST_CASE("non-crossing limits rest and set top of book", "[book]") {
    OrderBook book;
    REQUIRE(book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC).empty());
    REQUIRE(book.add_limit(2, Side::Sell, 101, 5, TimeInForce::GTC).empty());

    expect_top(book, std::optional<Price>{100}, std::optional<Price>{101});
    REQUIRE(book.order_count() == 2);
}

TEST_CASE("crossing limit trades at the resting maker price", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    const auto trades = book.add_limit(2, Side::Buy, 105, 5, TimeInForce::GTC);

    REQUIRE(trades.size() == 1);
    // executes at the resting maker price 100, not the aggressor's 105
    expect_trade(trades[0], /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/5);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("sell aggressor crosses resting bids", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    const auto trades = book.add_limit(2, Side::Sell, 99, 5, TimeInForce::GTC);

    REQUIRE(trades.size() == 1);
    expect_trade(trades[0], /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/5);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("price-time priority: earlier order at a level fills first", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC); // same price, later

    const auto trades = book.add_limit(3, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    expect_trade(trades[0], /*maker=*/1, /*taker=*/3, /*price=*/100,
                 /*quantity=*/5); // earlier first
    REQUIRE(book.quantity_at(Side::Sell, 100) == 5);
    REQUIRE(book.order_count() == 1);
}

TEST_CASE("partial fill leaves the maker remainder resting", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10, TimeInForce::GTC);

    const auto trades = book.add_limit(2, Side::Buy, 100, 4, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    expect_trade(trades[0], /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/4);
    REQUIRE(book.quantity_at(Side::Sell, 100) == 6); // remainder keeps resting
    REQUIRE(book.order_count() == 1);                // aggressor fully filled, did not rest
    REQUIRE(book.best_ask() == std::optional<Price>{100});
}

TEST_CASE("quantity_at is side-specific at the same price", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);

    REQUIRE(book.quantity_at(Side::Buy, 100) == 0);
    REQUIRE(book.quantity_at(Side::Sell, 100) == 5);
}

TEST_CASE("market order sweeps best levels until filled", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 3, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 101, 3, TimeInForce::GTC);

    const auto trades = book.add_market(3, Side::Buy, 5);
    REQUIRE(trades.size() == 2);
    expect_trade(trades[0], /*maker=*/1, /*taker=*/3, /*price=*/100, /*quantity=*/3);
    expect_trade(trades[1], /*maker=*/2, /*taker=*/3, /*price=*/101, /*quantity=*/2);
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
    expect_trade(trades[0], /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/2);
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
    expect_trade(trades[0], /*maker=*/1, /*taker=*/3, /*price=*/100, /*quantity=*/3); // still ahead
}

TEST_CASE("modify quantity increase loses time priority", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC);

    REQUIRE(book.modify(1, 100, 8).empty()); // qty increase -> requeued behind order 2
    const auto trades = book.add_limit(3, Side::Buy, 100, 5, TimeInForce::GTC);
    REQUIRE(trades.size() == 1);
    expect_trade(trades[0], /*maker=*/2, /*taker=*/3, /*price=*/100,
                 /*quantity=*/5); // order 2 first
}

TEST_CASE("modify price change loses priority and can cross", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 101, 5, TimeInForce::GTC);

    const auto trades = book.modify(2, 100, 5); // ask repriced down to 100 -> crosses the bid
    REQUIRE(trades.size() == 1);
    expect_trade(trades[0], /*maker=*/1, /*taker=*/2, /*price=*/100, /*quantity=*/5);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("book is never crossed after matching", "[book]") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    book.add_limit(2, Side::Sell, 102, 5, TimeInForce::GTC);
    // Aggressive buy below the ask does not cross; it rests as the new best bid.
    book.add_limit(3, Side::Buy, 101, 5, TimeInForce::GTC);

    expect_top(book, std::optional<Price>{101}, std::optional<Price>{102});
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
    expect_trade(first[0], /*maker=*/1, /*taker=*/3, /*price=*/100, /*quantity=*/3); // o1 partial

    const auto second = book.add_limit(4, Side::Buy, 100, 3, TimeInForce::GTC);
    REQUIRE(second.size() == 2);
    expect_trade(second[0], /*maker=*/1, /*taker=*/4, /*price=*/100,
                 /*quantity=*/2); // o1 remainder
    expect_trade(second[1], /*maker=*/2, /*taker=*/4, /*price=*/100, /*quantity=*/1); // then o2
}

TEST_CASE("resting_orders enumerates bids best-first then asks, FIFO within level",
          "[book][resting]") {
    for (const auto storage :
         {OrderBook::Storage::Baseline, OrderBook::Storage::Pooled,
          OrderBook::Storage::IntrusivePooled, OrderBook::Storage::Contiguous}) {
        CAPTURE(static_cast<int>(storage));
        OrderBook book{storage};
        book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
        book.add_limit(2, Side::Buy, 101, 3, TimeInForce::GTC); // better bid: listed first
        book.add_limit(3, Side::Buy, 101, 4, TimeInForce::GTC); // same level, later: after 2
        book.add_limit(4, Side::Sell, 103, 7, TimeInForce::GTC);
        book.add_limit(5, Side::Sell, 102, 6, TimeInForce::GTC); // better ask: listed first

        const std::vector<Order> expected{{2, Side::Buy, 101, 3},
                                          {3, Side::Buy, 101, 4},
                                          {1, Side::Buy, 100, 5},
                                          {5, Side::Sell, 102, 6},
                                          {4, Side::Sell, 103, 7}};
        REQUIRE(book.resting_orders() == expected);
    }
}

TEST_CASE("resting_orders reflects partial fills, cancels, and priority-losing modifies",
          "[book][resting]") {
    for (const auto storage :
         {OrderBook::Storage::Baseline, OrderBook::Storage::Pooled,
          OrderBook::Storage::IntrusivePooled, OrderBook::Storage::Contiguous}) {
        CAPTURE(static_cast<int>(storage));
        OrderBook book{storage};
        book.add_limit(1, Side::Sell, 100, 10, TimeInForce::GTC);
        book.add_limit(2, Side::Sell, 100, 5, TimeInForce::GTC);
        book.add_limit(3, Side::Sell, 100, 5, TimeInForce::GTC);

        book.add_limit(4, Side::Buy, 100, 4, TimeInForce::GTC); // partial-fills 1 down to 6
        book.modify(2, 100, 9);                                 // qty increase: 2 moves to back
        book.cancel(3);

        // 1 keeps its reduced remainder at the front; 2 lost priority and re-rested at the tail.
        const std::vector<Order> expected{{1, Side::Sell, 100, 6}, {2, Side::Sell, 100, 9}};
        REQUIRE(book.resting_orders() == expected);
    }
}

TEST_CASE("resting_orders on an empty book is empty", "[book][resting]") {
    OrderBook book;
    REQUIRE(book.resting_orders().empty());
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    book.cancel(1);
    REQUIRE(book.resting_orders().empty());
}

TEST_CASE("contiguous book refuses an out-of-band reprice and keeps the order", "[book]") {
    OrderBook book{OrderBook::Storage::Contiguous};
    book.add_limit(1, Side::Buy, 100, 5, TimeInForce::GTC);
    const Price out_of_band = OrderBook::kContiguousMaxPrice + 1;

    // No crossing liquidity: the re-add would have to rest out of band, so the modify is
    // refused and the original order is kept (not silently dropped).
    expect_modify_refused(book, out_of_band);
    expect_bid_order_kept(book, 1, 100);

    // In-band reprices, reductions, cancels (qty 0), and unknown ids all remain applicable.
    const bool applicable = book.can_apply_modify(1, 101, 9) && book.can_apply_modify(1, 100, 3) &&
                            book.can_apply_modify(1, out_of_band, 0) &&
                            book.can_apply_modify(999, out_of_band, 5);
    REQUIRE(applicable);
}

TEST_CASE("contiguous book refuses out-of-band residuals before matching", "[book]") {
    OrderBook book{OrderBook::Storage::Contiguous};
    const Price out_of_band = OrderBook::kContiguousMaxPrice + 1;

    REQUIRE(book.add_limit(1, Side::Sell, 100, 2, TimeInForce::GTC).empty());
    const auto trades = book.add_limit(2, Side::Buy, out_of_band, 5, TimeInForce::GTC);

    REQUIRE(trades.empty());
    expect_ask_order_kept(book, 1, 100);
    REQUIRE_FALSE(book.contains(2));
}

#include "qsl/replay/recovery.hpp"
#include "qsl/replay/shrink.hpp"

#include <catch2/catch_test_macros.hpp>
#include <variant>
#include <vector>

using namespace qsl;

namespace {
// Artificial failure predicate for demonstrating the shrinker: "the stream produces a trade".
// (In real differential use the predicate would be a C++/OCaml snapshot disagreement; the
// engines currently agree, so an artificial property is used.)
const replay::ShrinkPredicate produces_trade = [](const std::vector<replay::Command> &cmds) {
    return replay::count_trades(cmds, /*max_qty=*/20, /*max_notional=*/1000) > 0;
};
} // namespace

TEST_CASE("shrinker reduces a failing stream to a small, predicate-preserving counterexample",
          "[shrink]") {
    const auto original = replay::generate_property_flow(/*seed=*/1, /*symbols=*/3, /*orders=*/120);
    REQUIRE(produces_trade(original)); // the seed must actually trade for the demo to be valid

    const auto minimized = replay::shrink(original, produces_trade);

    REQUIRE(produces_trade(minimized));          // failure predicate preserved
    REQUIRE(minimized.size() < original.size()); // it actually shrank
    REQUIRE(minimized.size() <= 8);              // down to a small counterexample

    std::size_t iterations = 0;
    REQUIRE(replay::shrink(original, produces_trade, &iterations) == minimized);
    REQUIRE(iterations >= 1); // a real shrink performs at least one fixed-point pass
    // Removing any single command must break the predicate (1-minimal under removal).
    for (std::size_t i = 0; i < minimized.size(); ++i) {
        auto candidate = minimized;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
        REQUIRE_FALSE(produces_trade(candidate));
    }
}

TEST_CASE("shrinker simplifies prices", "[shrink]") {
    // A crossing pair at a high price; the "produces a trade" predicate is price-agnostic, so the
    // shrinker should lower both limit prices to 1 while preserving the trade.
    using namespace replay;
    const std::vector<Command> flow = {
        RegisterSymbol{"S0"},
        NewLimit{0, 1, core::Side::Sell, 500, 1, core::TimeInForce::GTC},
        NewLimit{0, 2, core::Side::Buy, 500, 1, core::TimeInForce::GTC},
    };
    REQUIRE(produces_trade(flow));

    const auto minimized = shrink(flow, produces_trade);
    REQUIRE(produces_trade(minimized));
    for (const auto &c : minimized) {
        if (const auto *nl = std::get_if<NewLimit>(&c)) {
            REQUIRE(nl->price == 1);
        }
    }
}

TEST_CASE("shrinker is deterministic", "[shrink]") {
    const auto original = replay::generate_property_flow(2, 3, 120);
    REQUIRE(produces_trade(original));
    REQUIRE(replay::shrink(original, produces_trade) == replay::shrink(original, produces_trade));
}

TEST_CASE("shrinker returns the input unchanged when the predicate does not hold", "[shrink]") {
    const std::vector<replay::Command> empty;
    REQUIRE_FALSE(produces_trade(empty));
    REQUIRE(replay::shrink(empty, produces_trade).empty());
}

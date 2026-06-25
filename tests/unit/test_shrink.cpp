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

TEST_CASE("shrinker renumbers symbols, dropping unreferenced registrations", "[shrink]") {
    // Only symbol 2 is used; the shrinker should drop the S0/S1 registrations and renumber the
    // surviving symbol to 0 so the minimal fixture carries a single registration.
    using namespace replay;
    const std::vector<Command> flow = {
        RegisterSymbol{"S0"},
        RegisterSymbol{"S1"},
        RegisterSymbol{"S2"},
        NewLimit{2, 7, core::Side::Sell, 100, 1, core::TimeInForce::GTC},
        NewLimit{2, 9, core::Side::Buy, 100, 1, core::TimeInForce::GTC},
    };
    REQUIRE(produces_trade(flow));

    const auto minimized = shrink(flow, produces_trade);
    REQUIRE(produces_trade(minimized));
    std::size_t registrations = 0;
    for (const auto &c : minimized) {
        if (std::holds_alternative<RegisterSymbol>(c)) {
            ++registrations;
        } else if (const auto *nl = std::get_if<NewLimit>(&c)) {
            REQUIRE(nl->symbol == 0); // the surviving symbol is renumbered to 0
        }
    }
    REQUIRE(registrations == 1);
}

TEST_CASE("renumber leaves idempotent duplicate registrations unchanged", "[shrink]") {
    // `reg X; reg X` allocates only symbol id 0 (registration is idempotent), so id 1 is never
    // registered and `limit 1 ...` is an UnknownSymbol reject. renumber must not assume the second
    // registration owns id 1 and "fix up" that order into an accepted one, it must bail.
    using namespace replay;
    const std::vector<Command> flow = {
        RegisterSymbol{"X"},
        RegisterSymbol{"X"},
        NewLimit{1, 5, core::Side::Sell, 100, 1, core::TimeInForce::GTC},
    };
    REQUIRE(renumber(flow) == flow); // unchanged: positional id model is unsafe here
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

// Differential oracle self-test (issue #34): the "fire alarm" test. The C++ and OCaml engines
// genuinely agree, so to prove the differential/shrink machinery actually catches and reduces a
// real divergence we inject a synthetic one: a deliberately buggy "mutant" replay that drops
// Cancel commands. The divergence predicate is "correct snapshot != mutant snapshot". We assert
// the divergence is detected, the shrinker reduces the failing stream, and the minimal stream
// still reproduces the divergence.

#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/recovery.hpp"
#include "qsl/replay/shrink.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <variant>
#include <vector>

using namespace qsl;

namespace {

// Replay commands through the gateway/engine and return the final snapshot. When `drop_cancels`
// is true this is the injected bug: Cancel commands are silently ignored.
engine::EngineSnapshot run(const std::vector<replay::Command> &cmds, bool drop_cancels) {
    engine::MatchingEngine eng;
    gateway::OrderGateway gw{eng, gateway::RiskConfig{/*max_qty=*/20, /*max_notional=*/1000}};
    for (const auto &c : cmds) {
        if (const auto *rs = std::get_if<replay::RegisterSymbol>(&c)) {
            eng.register_symbol(rs->name);
        } else if (const auto *nl = std::get_if<replay::NewLimit>(&c)) {
            static_cast<void>(
                gw.new_limit(nl->symbol, nl->id, nl->side, nl->price, nl->quantity, nl->tif));
        } else if (const auto *nm = std::get_if<replay::NewMarket>(&c)) {
            static_cast<void>(gw.new_market(nm->symbol, nm->id, nm->side, nm->quantity));
        } else if (const auto *cn = std::get_if<replay::Cancel>(&c)) {
            if (!drop_cancels) {
                static_cast<void>(gw.cancel(cn->symbol, cn->id));
            }
        } else {
            const auto &md = std::get<replay::Modify>(c);
            static_cast<void>(gw.modify(md.symbol, md.id, md.price, md.quantity));
        }
    }
    return eng.snapshot();
}

// The differential predicate: the correct engine and the cancel-dropping mutant disagree.
const replay::ShrinkPredicate diverges = [](const std::vector<replay::Command> &cmds) {
    return !(run(cmds, /*drop_cancels=*/false) == run(cmds, /*drop_cancels=*/true));
};

} // namespace

TEST_CASE("oracle self-test: an injected divergence is detected and shrunk to a minimal reproducer",
          "[selftest]") {
    // (1)+(2) Some seeded property flow must expose the injected mismatch (a cancel that
    // actually changes the final book), proving the predicate detects divergence.
    std::vector<replay::Command> original;
    for (std::uint64_t seed = 1; seed <= 20 && original.empty(); ++seed) {
        auto flow = replay::generate_property_flow(seed, /*symbols=*/3, /*orders=*/120);
        if (diverges(flow)) {
            original = std::move(flow);
        }
    }
    REQUIRE_FALSE(original.empty());

    // (3) The shrinker reduces the failing stream.
    const auto minimized = replay::shrink(original, diverges);
    REQUIRE(minimized.size() < original.size());
    REQUIRE(minimized.size() <= 6); // small, reviewable counterexample

    // (4) The minimal fixture still reproduces the divergence, and is 1-minimal under removal.
    REQUIRE(diverges(minimized));
    for (std::size_t i = 0; i < minimized.size(); ++i) {
        auto candidate = minimized;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
        REQUIRE_FALSE(diverges(candidate));
    }
}

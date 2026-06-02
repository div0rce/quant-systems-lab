#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <variant>
#include <vector>

using namespace qsl;
using core::OrderId;
using core::Quantity;
using core::QuantityTotal;
using core::SeqNo;
using core::Side;
using core::TimeInForce;

namespace {
bool not_crossed(const engine::EngineSnapshot &snap) {
    for (const auto &s : snap.symbols) {
        if (s.best_bid && s.best_ask && !(*s.best_bid < *s.best_ask)) {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("randomized flows preserve engine invariants", "[invariants]") {
    // Non-vacuity counters: prove the flow actually produced meaningful activity, so a
    // future generator change cannot silently turn these invariants into no-ops.
    std::uint64_t trades_seen = 0;
    std::uint64_t cancels_attempted = 0;
    std::uint64_t modifies_attempted = 0;
    bool ever_rested = false;

    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const auto flow = replay::generate_flow(seed, /*symbols=*/4, /*orders=*/2000);
        engine::MatchingEngine eng;
        SeqNo prev_seq = 0;

        for (const auto &cmd : flow) {
            OrderId taker = 0;
            Quantity submitted = 0;
            if (const auto *nl = std::get_if<replay::NewLimit>(&cmd)) {
                taker = nl->id;
                submitted = nl->quantity;
            } else if (const auto *nm = std::get_if<replay::NewMarket>(&cmd)) {
                taker = nm->id;
                submitted = nm->quantity;
            } else if (std::holds_alternative<replay::Cancel>(cmd)) {
                ++cancels_attempted;
            } else if (std::holds_alternative<replay::Modify>(cmd)) {
                ++modifies_attempted;
            }

            const auto events = replay::apply(eng, cmd);
            QuantityTotal executed = 0;
            for (const auto &ev : events) {
                const SeqNo seq = engine::seq_of(ev);
                REQUIRE(seq > prev_seq); // (7) sequence numbers strictly increase
                prev_seq = seq;
                if (const auto *t = std::get_if<engine::TradeEvent>(&ev)) {
                    REQUIRE(t->quantity > 0); // (1) no zero/negative trade quantity
                    ++trades_seen;
                    if (t->taker_id == taker) {
                        executed += t->quantity;
                    }
                }
            }
            if (taker != 0) {
                REQUIRE(executed <= submitted); // (3) executed cannot exceed submitted
            }

            const auto snap = eng.snapshot();
            REQUIRE(not_crossed(snap)); // (2) no crossed book after matching
            for (const auto &s : snap.symbols) {
                if (s.order_count > 0) {
                    ever_rested = true;
                }
                for (const auto &lv : s.bids) {
                    REQUIRE(lv.quantity > 0); // (1) no zero-quantity resting levels
                }
                for (const auto &lv : s.asks) {
                    REQUIRE(lv.quantity > 0);
                }
            }
        }
    }

    // Non-vacuity: the fixed seeds must have exercised the interesting paths.
    REQUIRE(trades_seen > 0);
    REQUIRE(ever_rested);
    REQUIRE(cancels_attempted > 0);
    REQUIRE(modifies_attempted > 0);
}

TEST_CASE("a risk-rejected order never rests in the book", "[invariants]") {
    engine::MatchingEngine eng;
    const auto a = eng.register_symbol("AAPL");
    gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};

    REQUIRE_FALSE(gw.new_limit(a, 1, Side::Buy, 0, 5, TimeInForce::GTC).accepted);      // bad price
    REQUIRE_FALSE(gw.new_limit(a, 2, Side::Buy, 100, 5000, TimeInForce::GTC).accepted); // max qty
    REQUIRE_FALSE(gw.new_limit(99, 3, Side::Buy, 100, 5, TimeInForce::GTC).accepted); // bad symbol

    REQUIRE_FALSE(eng.contains(a, 1)); // (5) rejected order cannot rest
    REQUIRE_FALSE(eng.contains(a, 2));
    REQUIRE(eng.snapshot().symbols[0].order_count == 0);
}

TEST_CASE("a canceled order cannot trade afterward", "[invariants]") {
    engine::MatchingEngine eng;
    const auto a = eng.register_symbol("AAPL");
    static_cast<void>(eng.new_limit(a, 1, Side::Sell, 100, 5, TimeInForce::GTC)); // front
    static_cast<void>(eng.new_limit(a, 2, Side::Sell, 100, 5, TimeInForce::GTC)); // behind
    REQUIRE(eng.cancel(a, 1).size() == 1);

    const auto events = eng.new_limit(a, 3, Side::Buy, 100, 5, TimeInForce::GTC); // crosses
    bool canceled_traded = false;
    for (const auto &ev : events) {
        if (const auto *t = std::get_if<engine::TradeEvent>(&ev)) {
            if (t->maker_id == 1) {
                canceled_traded = true; // (4) canceled order cannot later trade
            }
        }
    }
    REQUIRE_FALSE(canceled_traded);
}

TEST_CASE("replay reproduces final state under stress across seeds", "[invariants]") {
    for (const std::uint64_t seed : {11U, 22U, 33U}) {
        const auto flow = replay::generate_flow(seed, /*symbols=*/5, /*orders=*/8000);
        engine::MatchingEngine original;
        std::vector<replay::LogRecord> records;
        std::uint64_t i = 0;
        for (const auto &cmd : flow) {
            static_cast<void>(replay::apply(original, cmd));
            records.push_back({i, replay::RecordType::Command, i, replay::encode_command(cmd)});
            ++i;
        }
        engine::MatchingEngine rebuilt;
        static_cast<void>(replay::replay(rebuilt, records));
        REQUIRE(rebuilt.snapshot() == original.snapshot()); // (6) replay == original
    }
}

TEST_CASE("pool-backed order-book storage matches baseline exactly", "[invariants][m32]") {
    // M32: the OrderBook storage mode (Baseline = operator new/delete vs Pooled = a per-book
    // std::pmr::unsynchronized_pool_resource) must not change any observable result -- it only
    // changes where nodes live in memory. Replay the same generated flow through both modes and
    // require identical emitted event streams, last sequence numbers, and final snapshots.
    bool any_trades = false;
    bool any_rested = false;

    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        const auto flow = replay::generate_flow(seed, /*symbols=*/4, /*orders=*/2000);
        engine::MatchingEngine baseline; // Storage::Baseline (default)
        engine::MatchingEngine pooled{engine::OrderBook::Storage::Pooled};

        std::vector<engine::EngineEvent> base_stream;
        std::vector<engine::EngineEvent> pool_stream;
        for (const auto &cmd : flow) {
            const auto be = replay::apply(baseline, cmd);
            const auto pe = replay::apply(pooled, cmd);
            REQUIRE(be == pe); // per-command event streams are identical
            for (const auto &e : be) {
                base_stream.push_back(e);
                if (std::holds_alternative<engine::TradeEvent>(e)) {
                    any_trades = true;
                }
            }
            for (const auto &e : pe) {
                pool_stream.push_back(e);
            }
        }

        REQUIRE(base_stream == pool_stream);
        REQUIRE(baseline.last_seq() == pooled.last_seq());
        REQUIRE(baseline.snapshot() == pooled.snapshot());
        for (const auto &s : baseline.snapshot().symbols) {
            if (s.order_count > 0) {
                any_rested = true;
            }
        }
    }

    // Non-vacuity: the flow must actually exercise matching and leave resting liquidity, so this
    // equivalence cannot silently degrade into comparing two empty books.
    REQUIRE(any_trades);
    REQUIRE(any_rested);
}

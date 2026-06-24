#include "qsl/engine/matching_engine.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <variant>
#include <vector>

using namespace qsl::replay;
using namespace qsl::engine;

namespace {

struct FlowChurnCoverage {
    std::set<SymbolId> touched_symbols;
    std::size_t limits = 0;
    std::size_t ioc_limits = 0;
    std::size_t markets = 0;
    std::size_t cancels = 0;
    std::size_t modifies = 0;
    std::size_t active_cancel_targets = 0;
    std::size_t active_modify_targets = 0;
    std::size_t trades = 0;

    [[nodiscard]] bool is_market_like() const {
        const std::array checks{touched_symbols.size() > 1,
                                limits > 0,
                                ioc_limits > 0,
                                markets > 0,
                                cancels > 0,
                                modifies > 0,
                                active_cancel_targets > 0,
                                active_modify_targets > 0,
                                trades > 0};
        return std::all_of(checks.begin(), checks.end(), [](bool ok) { return ok; });
    }
};

void observe_command(const Command &command, const MatchingEngine &model,
                     FlowChurnCoverage &coverage) {
    if (const auto *limit = std::get_if<NewLimit>(&command)) {
        coverage.touched_symbols.insert(limit->symbol);
        ++coverage.limits;
        coverage.ioc_limits += limit->tif == TimeInForce::IOC ? 1U : 0U;
        return;
    }
    if (const auto *market = std::get_if<NewMarket>(&command)) {
        coverage.touched_symbols.insert(market->symbol);
        ++coverage.markets;
        return;
    }
    if (const auto *cancel = std::get_if<Cancel>(&command)) {
        coverage.touched_symbols.insert(cancel->symbol);
        coverage.active_cancel_targets += model.contains(cancel->symbol, cancel->id) ? 1U : 0U;
        ++coverage.cancels;
        return;
    }
    if (const auto *modify = std::get_if<Modify>(&command)) {
        coverage.touched_symbols.insert(modify->symbol);
        coverage.active_modify_targets += model.contains(modify->symbol, modify->id) ? 1U : 0U;
        ++coverage.modifies;
    }
}

void observe_events(const std::vector<EngineEvent> &events, FlowChurnCoverage &coverage) {
    for (const auto &event : events) {
        coverage.trades += std::holds_alternative<TradeEvent>(event) ? 1U : 0U;
    }
}

FlowChurnCoverage measure_churn(const std::vector<Command> &flow) {
    MatchingEngine model;
    FlowChurnCoverage coverage;
    for (const auto &command : flow) {
        observe_command(command, model, coverage);
        observe_events(qsl::replay::apply(model, command), coverage);
    }
    return coverage;
}

} // namespace

TEST_CASE("replay rebuilds identical final state and event sequence", "[replay]") {
    const auto flow = generate_flow(/*seed=*/7, /*symbols=*/3, /*orders=*/400);

    // Original run: drive the engine and record each command to a log.
    MatchingEngine original;
    std::vector<EngineEvent> original_events;
    std::vector<LogRecord> records;
    std::uint64_t i = 0;
    for (const auto &command : flow) {
        const auto produced = apply(original, command);
        original_events.insert(original_events.end(), produced.begin(), produced.end());
        records.push_back(LogRecord{i, RecordType::CommandRecord, i, encode_command(command)});
        ++i;
    }

    // The flow must actually exercise the engine for the comparison to be meaningful.
    REQUIRE(original.last_seq() > 0);
    bool saw_trade = false;
    for (const auto &ev : original_events) {
        if (std::holds_alternative<TradeEvent>(ev)) {
            saw_trade = true;
        }
    }
    REQUIRE(saw_trade);

    // Replay run: rebuild a fresh engine purely from the log.
    MatchingEngine rebuilt;
    const auto replay_events = replay(rebuilt, records);

    REQUIRE(rebuilt.snapshot() == original.snapshot()); // best bid/ask, levels, counts, last_seq
    REQUIRE(replay_events == original_events);          // emitted trade/event sequence
}

TEST_CASE("file-backed replay uses event-log framing before rebuilding state", "[replay]") {
    const auto flow = generate_flow(/*seed=*/17, /*symbols=*/3, /*orders=*/300);
    const auto path = std::filesystem::temp_directory_path() / "qsl_replay_file_level.bin";
    std::filesystem::remove(path);

    MatchingEngine original;
    std::vector<EngineEvent> original_events;
    {
        EventLogWriter writer(path);
        REQUIRE(writer.good());
        std::uint64_t i = 0;
        for (const auto &command : flow) {
            const auto produced = apply(original, command);
            original_events.insert(original_events.end(), produced.begin(), produced.end());
            REQUIRE(
                writer.append(LogRecord{i, RecordType::CommandRecord, i, encode_command(command)}));
            ++i;
        }
    }

    const auto log = EventLogReader(path).read_all();
    REQUIRE(log.error == LogError::None);
    REQUIRE(log.records.size() == flow.size());

    MatchingEngine rebuilt;
    const auto replay_events = replay(rebuilt, log.records);
    REQUIRE(rebuilt.snapshot() == original.snapshot());
    REQUIRE(replay_events == original_events);

    std::filesystem::remove(path);
}

TEST_CASE("synthetic flow is deterministic in its seed", "[replay]") {
    REQUIRE(generate_flow(7, 3, 300) == generate_flow(7, 3, 300));
    REQUIRE_FALSE(generate_flow(7, 3, 300) == generate_flow(8, 3, 300));
}

TEST_CASE("synthetic flow includes market-like order churn", "[replay]") {
    const auto flow = generate_flow(/*seed=*/11, /*symbols=*/4, /*orders=*/1000);
    REQUIRE(measure_churn(flow).is_market_like());
}

TEST_CASE("commands round-trip through the codec", "[replay]") {
    const std::vector<Command> commands{
        RegisterSymbol{"AAPL"},         NewLimit{1, 2, Side::Buy, 12345, 10, TimeInForce::IOC},
        NewMarket{1, 3, Side::Sell, 7}, Cancel{1, 2},
        Modify{1, 2, 9999, 5},
    };
    for (const auto &command : commands) {
        const auto bytes = encode_command(command);
        const auto decoded = decode_command(bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == command);
    }
}

TEST_CASE("decode_command rejects malformed payloads", "[replay]") {
    REQUIRE_FALSE(decode_command(std::span<const std::byte>{}).has_value());

    const std::vector<std::byte> unknown_tag{static_cast<std::byte>(0xFF)};
    REQUIRE_FALSE(decode_command(unknown_tag).has_value());

    auto truncated = encode_command(Command{NewLimit{1, 2, Side::Buy, 100, 5, TimeInForce::GTC}});
    truncated.pop_back();
    REQUIRE_FALSE(decode_command(truncated).has_value());

    // Out-of-domain enum bytes must be refused, not silently applied: the replay path feeds decoded
    // commands straight to the engine (no gateway risk check), so a corrupt Side/TIF byte would
    // otherwise diverge replayed state. Side is {0,1}, TimeInForce is {0,1}.
    auto bad_limit_side =
        encode_command(Command{NewLimit{1, 2, Side::Buy, 100, 5, TimeInForce::GTC}});
    bad_limit_side[25] = static_cast<std::byte>(0x02);
    REQUIRE_FALSE(decode_command(bad_limit_side).has_value());

    auto bad_limit_tif =
        encode_command(Command{NewLimit{1, 2, Side::Buy, 100, 5, TimeInForce::GTC}});
    bad_limit_tif[26] = static_cast<std::byte>(0x07);
    REQUIRE_FALSE(decode_command(bad_limit_tif).has_value());

    auto bad_market_side = encode_command(Command{NewMarket{1, 3, Side::Sell, 7}});
    bad_market_side[13] = static_cast<std::byte>(0xAA);
    REQUIRE_FALSE(decode_command(bad_market_side).has_value());
}

TEST_CASE("snapshot reports aggregate per-level quantities", "[replay]") {
    const std::vector<Command> flow{
        RegisterSymbol{"X"},
        NewLimit{0, 1, Side::Buy, 100, 5, TimeInForce::GTC},
        NewLimit{0, 2, Side::Buy, 100, 3, TimeInForce::GTC}, // same level -> aggregates to 8
        NewLimit{0, 3, Side::Buy, 99, 4, TimeInForce::GTC},
        NewLimit{0, 4, Side::Sell, 101, 6, TimeInForce::GTC},
    };
    MatchingEngine engine;
    for (const auto &command : flow) {
        static_cast<void>(apply(engine, command));
    }

    const auto snapshot = engine.snapshot();
    REQUIRE(snapshot.symbols.size() == 1);
    const auto &s = snapshot.symbols[0];
    REQUIRE(s.bids.size() == 2);
    REQUIRE(s.bids[0] == LevelView{100, 8}); // best (highest) first, quantities aggregated
    REQUIRE(s.bids[1] == LevelView{99, 4});
    REQUIRE(s.asks.size() == 1);
    REQUIRE(s.asks[0] == LevelView{101, 6});
}

TEST_CASE("non-command records are skipped on replay", "[replay]") {
    std::vector<LogRecord> records{
        LogRecord{0, RecordType::CommandRecord, 0, encode_command(Command{RegisterSymbol{"X"}})},
        LogRecord{1, RecordType::EventRecord, 1, {}}, // an event record: ignored by replay
        LogRecord{2, RecordType::CommandRecord, 2,
                  encode_command(Command{NewLimit{0, 1, Side::Buy, 100, 5, TimeInForce::GTC}})},
    };
    MatchingEngine engine;
    static_cast<void>(replay(engine, records));
    REQUIRE(engine.best_bid(0) == std::optional<Price>{100});
    REQUIRE(engine.snapshot().symbols.size() == 1);
}

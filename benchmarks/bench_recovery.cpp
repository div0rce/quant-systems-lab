#include "qsl/engine/matching_engine.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace qsl::bench {
namespace {

volatile std::uint64_t g_recovery_sink = 0;

using clock_type = std::chrono::steady_clock;

void escape(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(p) : "memory");
#else
    (void)p;
#endif
}

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "bench recovery: %s\n", message);
    std::exit(1);
}

// The state a snapshot design would persist: symbol names in registration order plus every
// resting order in priority order (see OrderBook::resting_orders). Sequencing state is *not*
// restorable through the public engine API; the restore below rebuilds book state only.
struct CapturedState {
    std::vector<std::string> names;
    std::vector<std::vector<engine::Order>> orders_per_symbol;
};

CapturedState capture_state(const engine::MatchingEngine &eng,
                            const std::vector<std::string> &names) {
    CapturedState state;
    state.names = names;
    state.orders_per_symbol.reserve(names.size());
    for (core::SymbolId symbol = 0; symbol < static_cast<core::SymbolId>(names.size()); ++symbol) {
        state.orders_per_symbol.push_back(eng.resting_orders(symbol));
    }
    return state;
}

// Rebuild book state by re-adding the captured orders in priority order. Uncrossed state
// re-added per symbol (bids first, then asks) never trades, so every insert is a plain accept.
engine::MatchingEngine restore_state(const CapturedState &state) {
    engine::MatchingEngine eng;
    for (std::size_t i = 0; i < state.names.size(); ++i) {
        const core::SymbolId symbol = eng.register_symbol(state.names[i]);
        for (const engine::Order &order : state.orders_per_symbol[i]) {
            g_recovery_sink += eng.new_limit(symbol, order.id, order.side, order.price,
                                             order.quantity, core::TimeInForce::GTC)
                                   .size();
        }
    }
    return eng;
}

// Time `op` over `reps` repetitions after one warmup call; returns average seconds per call.
template <class F> double timed(std::size_t reps, F op) {
    op();
    const auto t0 = clock_type::now();
    for (std::size_t r = 0; r < reps; ++r) {
        op();
    }
    const auto t1 = clock_type::now();
    return std::chrono::duration<double>(t1 - t0).count() / static_cast<double>(reps);
}

void report(const char *name, std::size_t reps, double sec_per_run, std::size_t items,
            const char *item_unit) {
    std::printf("  %-42s %4zu reps %12.3f ms/run %12.1f ns/%s\n", name, reps, sec_per_run * 1e3,
                (sec_per_run * 1e9) / static_cast<double>(items), item_unit);
}

void bench_recovery_at_size(const std::filesystem::path &dir, std::uint64_t seed,
                            core::SymbolId symbols, std::size_t orders, std::size_t reps) {
    const auto flow = replay::generate_flow(seed, symbols, orders);

    std::vector<std::string> names;
    engine::MatchingEngine reference;
    for (const auto &command : flow) {
        if (const auto *reg = std::get_if<replay::RegisterSymbol>(&command)) {
            names.push_back(reg->name);
        }
        g_recovery_sink += replay::apply(reference, command).size();
    }
    const auto reference_snapshot = reference.snapshot();

    const std::filesystem::path log_path = dir / ("flow_" + std::to_string(orders) + ".qlog");
    {
        replay::EventLogWriter writer{log_path};
        if (!writer.good()) {
            fail("could not open benchmark log file for writing");
        }
        std::uint64_t seq = 0;
        for (const auto &command : flow) {
            const replay::LogRecord record{seq, replay::RecordType::CommandRecord, seq,
                                           replay::encode_command(command)};
            if (!writer.append(record)) {
                fail("log append failed while preparing the benchmark file");
            }
            ++seq;
        }
    }
    const auto file_bytes = std::filesystem::file_size(log_path);

    std::size_t resting = 0;
    for (const auto &symbol : reference_snapshot.symbols) {
        resting += symbol.order_count;
    }
    std::printf("log %zu commands  file %.2f MiB  resting %zu orders\n", flow.size(),
                static_cast<double>(file_bytes) / (1024.0 * 1024.0), resting);

    // Phase A: read the file back and verify/classify every frame (the M45 crash-safe entry
    // point). This is the disk + framing share of a restart.
    const double recover_sec = timed(reps, [&] {
        const auto recovery = replay::recover_log_file(log_path);
        if (recovery.error != replay::LogError::None ||
            recovery.tail != replay::TailState::CleanTail ||
            recovery.records.size() != flow.size()) {
            fail("recover_log_file did not return the clean full log");
        }
        g_recovery_sink += recovery.records.size();
    });
    report("recover_log_file (read+verify+classify)", reps, recover_sec, flow.size(), "record");

    // Phase B: decode + apply every command into a fresh engine (the in-memory replay share).
    const auto recovery = replay::recover_log_file(log_path);
    const double replay_sec = timed(reps, [&] {
        engine::MatchingEngine eng;
        g_recovery_sink += replay::replay(eng, recovery.records).size();
        escape(&eng);
    });
    {
        engine::MatchingEngine eng;
        g_recovery_sink += replay::replay(eng, recovery.records).size();
        if (eng.snapshot() != reference_snapshot) {
            fail("replayed engine state does not match the reference snapshot");
        }
    }
    report("replay into fresh engine (decode+apply)", reps, replay_sec, flow.size(), "command");

    // Phase C: the full restart path a process would actually run after a crash.
    const double restart_sec = timed(reps, [&] {
        const auto run = replay::recover_log_file(log_path);
        engine::MatchingEngine eng;
        g_recovery_sink += replay::replay(eng, run.records).size();
        escape(&eng);
    });
    report("full restart (recover + replay)", reps, restart_sec, flow.size(), "command");

    // Snapshot-restoration prototype (benchmark-only; no snapshot persistence exists in the
    // repo). Capture = enumerate the live resting state; restore = rebuild the book from it.
    // Both are in-memory, so they are a lower bound: a real snapshot path would also pay
    // serialization, disk I/O, and tail replay of the log past the snapshot point.
    const std::size_t state_items = resting == 0 ? 1 : resting;
    const double capture_sec = timed(reps, [&] {
        const auto state = capture_state(reference, names);
        g_recovery_sink += state.orders_per_symbol.size();
        escape(&state);
    });
    report("capture resting state (snapshot proto)", reps, capture_sec, state_items, "order");

    const auto state = capture_state(reference, names);
    const double restore_sec = timed(reps, [&] {
        const auto eng = restore_state(state);
        g_recovery_sink += eng.last_seq();
        escape(&eng);
    });
    {
        const auto rebuilt = restore_state(state);
        if (rebuilt.snapshot().symbols != reference_snapshot.symbols) {
            fail("rebuilt book state does not match the reference snapshot");
        }
        for (core::SymbolId symbol = 0; symbol < static_cast<core::SymbolId>(names.size());
             ++symbol) {
            if (rebuilt.resting_orders(symbol) != reference.resting_orders(symbol)) {
                fail("rebuilt resting-order priority does not match the reference");
            }
        }
    }
    report("rebuild book from captured state", reps, restore_sec, state_items, "order");
    std::printf("\n");
}

// Restoration cost scales with live state, not history. The generated flow above leaves only a
// few dozen resting orders, so this section constructs synthetic resting books of controlled
// depth (non-crossing GTC limits, never traded) to measure capture/rebuild per order at depth.
void bench_rebuild_at_depth(core::SymbolId symbols, std::size_t total_orders, std::size_t reps) {
    std::vector<std::string> names;
    names.reserve(symbols);
    engine::MatchingEngine reference;
    for (core::SymbolId s = 0; s < symbols; ++s) {
        names.push_back("SYM" + std::to_string(s));
        g_recovery_sink += reference.register_symbol(names.back());
    }
    for (std::size_t i = 0; i < total_orders; ++i) {
        const auto symbol = static_cast<core::SymbolId>(i % symbols);
        const bool buy = (i / symbols) % 2 == 0;
        // Bids in [50, 99], asks in [101, 150]: the book can never cross.
        const core::Price price =
            buy ? 50 + static_cast<core::Price>(i % 50) : 101 + static_cast<core::Price>(i % 50);
        const auto quantity = static_cast<core::Quantity>(1 + i % 100);
        const auto events = reference.new_limit(symbol, static_cast<core::OrderId>(i + 1),
                                                buy ? core::Side::Buy : core::Side::Sell, price,
                                                quantity, core::TimeInForce::GTC);
        if (events.size() != 1) {
            fail("synthetic resting book unexpectedly traded or rejected");
        }
    }

    std::printf("synthetic resting book  %u symbols  %zu resting orders\n",
                static_cast<unsigned>(symbols), total_orders);

    const double capture_sec = timed(reps, [&] {
        const auto state = capture_state(reference, names);
        g_recovery_sink += state.orders_per_symbol.size();
        escape(&state);
    });
    report("capture resting state (snapshot proto)", reps, capture_sec, total_orders, "order");

    const auto state = capture_state(reference, names);
    const double restore_sec = timed(reps, [&] {
        const auto eng = restore_state(state);
        g_recovery_sink += eng.last_seq();
        escape(&eng);
    });
    {
        const auto rebuilt = restore_state(state);
        if (rebuilt.snapshot().symbols != reference.snapshot().symbols) {
            fail("rebuilt synthetic book state does not match the reference");
        }
    }
    report("rebuild book from captured state", reps, restore_sec, total_orders, "order");
    std::printf("\n");
}

} // namespace

void run_recovery_benchmarks() {
    constexpr std::uint64_t kSeed = 42;
    constexpr core::SymbolId kSymbols = 4;
    constexpr std::size_t kReps = 10;

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "qsl-bench-recovery";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (!std::filesystem::create_directories(dir)) {
        fail("could not create the benchmark temp directory");
    }

    for (const std::size_t orders :
         {std::size_t{5'000}, std::size_t{20'000}, std::size_t{80'000}}) {
        bench_recovery_at_size(dir, kSeed, kSymbols, orders, kReps);
    }
    for (const std::size_t depth : {std::size_t{1'000}, std::size_t{10'000}, std::size_t{50'000}}) {
        bench_rebuild_at_depth(kSymbols, depth, kReps);
    }

    std::filesystem::remove_all(dir, ec);
    if (g_recovery_sink == 0) {
        fail("benchmark sink is unexpectedly zero");
    }
}

} // namespace qsl::bench

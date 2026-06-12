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

struct Timing {
    std::size_t reps;
    double sec_per_run;
};

// Time `op` over `reps` repetitions after one warmup call; reports average seconds per call.
template <class F> Timing timed(std::size_t reps, F op) {
    op();
    const auto t0 = clock_type::now();
    for (std::size_t r = 0; r < reps; ++r) {
        op();
    }
    const auto t1 = clock_type::now();
    return {reps, std::chrono::duration<double>(t1 - t0).count() / static_cast<double>(reps)};
}

void report(const char *name, Timing timing, std::size_t items, const char *item_unit) {
    std::printf("  %-42s %4zu reps %12.3f ms/run %12.1f ns/%s\n", name, timing.reps,
                timing.sec_per_run * 1e3, (timing.sec_per_run * 1e9) / static_cast<double>(items),
                item_unit);
}

bool clean_full_log(const replay::LogRecovery &recovery, std::size_t expected_records) {
    return recovery.error == replay::LogError::None &&
           recovery.tail == replay::TailState::CleanTail &&
           recovery.records.size() == expected_records;
}

std::filesystem::path write_flow_log(const std::filesystem::path &dir,
                                     const std::vector<replay::Command> &flow) {
    const std::filesystem::path path = dir / ("flow_" + std::to_string(flow.size()) + ".qlog");
    replay::EventLogWriter writer{path};
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
    return path;
}

// A reference engine built from a generated flow, plus everything the phases verify against.
struct FlowReference {
    std::vector<std::string> names;
    engine::MatchingEngine engine;
    engine::EngineSnapshot snapshot;
    std::size_t resting = 0;
};

FlowReference build_reference(const std::vector<replay::Command> &flow) {
    FlowReference ref;
    for (const auto &command : flow) {
        if (const auto *reg = std::get_if<replay::RegisterSymbol>(&command)) {
            ref.names.push_back(reg->name);
        }
        g_recovery_sink += replay::apply(ref.engine, command).size();
    }
    ref.snapshot = ref.engine.snapshot();
    for (const auto &symbol : ref.snapshot.symbols) {
        ref.resting += symbol.order_count;
    }
    return ref;
}

// Phases A-C: the restart path a process would actually run after a crash, split into the
// disk + framing share (recover_log_file, the M45 crash-safe entry point), the in-memory
// decode + apply share, and the combined end-to-end restart.
void bench_restart_phases(const std::filesystem::path &log_path, std::size_t commands,
                          const engine::EngineSnapshot &reference_snapshot, std::size_t reps) {
    const Timing recover = timed(reps, [&] {
        const auto recovery = replay::recover_log_file(log_path);
        if (!clean_full_log(recovery, commands)) {
            fail("recover_log_file did not return the clean full log");
        }
        g_recovery_sink += recovery.records.size();
    });
    report("recover_log_file (read+verify+classify)", recover, commands, "record");

    const auto recovery = replay::recover_log_file(log_path);
    const Timing replayed = timed(reps, [&] {
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
    report("replay into fresh engine (decode+apply)", replayed, commands, "command");

    const Timing restart = timed(reps, [&] {
        const auto run = replay::recover_log_file(log_path);
        engine::MatchingEngine eng;
        g_recovery_sink += replay::replay(eng, run.records).size();
        escape(&eng);
    });
    report("full restart (recover + replay)", restart, commands, "command");
}

// Book state must match the reference exactly, including intra-level FIFO priority; abort
// rather than report numbers from a wrong rebuild. Sequencing state is intentionally not
// compared: the rebuilt engine re-counts accepts (see CapturedState).
void verify_rebuilt_state(const CapturedState &state, const engine::MatchingEngine &reference) {
    const auto rebuilt = restore_state(state);
    if (rebuilt.snapshot().symbols != reference.snapshot().symbols) {
        fail("rebuilt book state does not match the reference snapshot");
    }
    for (core::SymbolId symbol = 0; symbol < static_cast<core::SymbolId>(state.names.size());
         ++symbol) {
        if (rebuilt.resting_orders(symbol) != reference.resting_orders(symbol)) {
            fail("rebuilt resting-order priority does not match the reference");
        }
    }
}

// Snapshot-restoration prototype (benchmark-only; no snapshot persistence exists in the
// repo). Capture = enumerate the live resting state; restore = rebuild the book from it.
// Both are in-memory, so they are a lower bound: a real snapshot path would also pay
// serialization, disk I/O, and tail replay of the log past the snapshot point.
void bench_capture_and_rebuild(const engine::MatchingEngine &reference,
                               const std::vector<std::string> &names, std::size_t orders,
                               std::size_t reps) {
    const std::size_t items = orders == 0 ? 1 : orders;
    const Timing capture = timed(reps, [&] {
        const auto state = capture_state(reference, names);
        g_recovery_sink += state.orders_per_symbol.size();
        escape(&state);
    });
    report("capture resting state (snapshot proto)", capture, items, "order");

    const auto state = capture_state(reference, names);
    const Timing restore = timed(reps, [&] {
        const auto eng = restore_state(state);
        g_recovery_sink += eng.last_seq();
        escape(&eng);
    });
    verify_rebuilt_state(state, reference);
    report("rebuild book from captured state", restore, items, "order");
}

struct FlowScenario {
    std::uint64_t seed;
    core::SymbolId symbols;
    std::size_t reps;
};

void bench_recovery_at_size(const std::filesystem::path &dir, FlowScenario scenario,
                            std::size_t orders) {
    const auto flow = replay::generate_flow(scenario.seed, scenario.symbols, orders);
    const FlowReference ref = build_reference(flow);
    const std::filesystem::path log_path = write_flow_log(dir, flow);
    const auto file_bytes = std::filesystem::file_size(log_path);

    std::printf("log %zu commands  file %.2f MiB  resting %zu orders\n", flow.size(),
                static_cast<double>(file_bytes) / (1024.0 * 1024.0), ref.resting);
    bench_restart_phases(log_path, flow.size(), ref.snapshot, scenario.reps);
    bench_capture_and_rebuild(ref.engine, ref.names, ref.resting, scenario.reps);
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
    bench_capture_and_rebuild(reference, names, total_orders, reps);
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
        bench_recovery_at_size(dir, FlowScenario{kSeed, kSymbols, kReps}, orders);
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

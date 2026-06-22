#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/order_book.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/session.hpp"
#include "qsl/protocol/codec.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/dispatch.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"
#include "qsl/replay/shrink.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace qsl::bench {
void run_order_pool_benchmarks();
void run_storage_benchmarks();
void run_false_sharing_benchmarks();
void run_recovery_benchmarks();
} // namespace qsl::bench

namespace {

// Sink to stop the optimizer from discarding benchmarked work. Wall-clock timing is fine at
// the benchmark layer (it is never used inside the deterministic engine).
volatile std::uint64_t g_sink = 0;

using clock_type = std::chrono::steady_clock;
using RecordType = qsl::replay::RecordType;

// Per-operation latency: run `iters` ops, report ns/op and ops/sec.
template <class F> void latency(const char *name, std::size_t iters, F op) {
    for (std::size_t i = 0; i < iters / 10 + 1; ++i) {
        op();
    }
    const auto t0 = clock_type::now();
    for (std::size_t i = 0; i < iters; ++i) {
        op();
    }
    const auto t1 = clock_type::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double per = ns / static_cast<double>(iters);
    std::printf("%-26s %9zu ops   %10.1f ns/op   %12.0f ops/sec\n", name, iters, per, 1e9 / per);
}

// Throughput: run `run` (which processes `items` items) `reps` times; report items/sec.
template <class F> void throughput(const char *name, std::size_t items, std::size_t reps, F run) {
    run(); // warmup
    const auto t0 = clock_type::now();
    for (std::size_t i = 0; i < reps; ++i) {
        run();
    }
    const auto t1 = clock_type::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double total = static_cast<double>(items * reps);
    std::printf("%-26s %9zu items %10.1f ns/item %12.0f items/sec\n", name, items,
                (sec * 1e9) / total, total / sec);
}

// Differential-testing harness benchmarks: the cost of generating, replaying (through the risk
// gateway, as the differential does), and shrinking property command streams.
void run_diff_benchmarks() {
    using namespace qsl;

    // 1. Property command-stream generation (generate_property_flow, varied seeds).
    {
        std::uint64_t seed = 1;
        const std::size_t items = replay::generate_property_flow(1, 3, 120).size();
        throughput("property flow generation", items, 200, [&] {
            const auto flow = replay::generate_property_flow(seed++, 3, 120);
            g_sink += flow.size();
        });
    }

    // 2. Differential replay: apply a property flow through the risk gateway + engine (the
    //    path the C++ side of the differential exercises), including risk rejections.
    {
        const auto flow = replay::generate_property_flow(1, 3, 120);
        throughput("differential gateway replay", flow.size(), 200, [&] {
            engine::MatchingEngine eng;
            gateway::OrderGateway gw{eng, gateway::RiskConfig{20, 1000}};
            for (const auto &cmd : flow) {
                g_sink += replay::apply_command(eng, gw, cmd).events.size();
            }
            g_sink += eng.last_seq();
        });
    }

    // 3. Shrink: delta-debug a failing property flow to a fixed point (re-running the predicate
    //    on each candidate). Reported as latency per full shrink.
    {
        const auto flow = replay::generate_property_flow(1, 3, 120);
        const replay::ShrinkPredicate produces_trade =
            [](const std::vector<replay::Command> &cmds) {
                return replay::count_trades(cmds, 20, 1000) > 0;
            };
        latency("shrink property flow", 300,
                [&] { g_sink += replay::shrink(flow, produces_trade).size(); });
    }
}

const char *storage_name(qsl::engine::OrderBook::Storage s) {
    switch (s) {
    case qsl::engine::OrderBook::Storage::Baseline:
        return "baseline";
    case qsl::engine::OrderBook::Storage::Pooled:
        return "pooled";
    case qsl::engine::OrderBook::Storage::IntrusivePooled:
        return "intrusive";
    case qsl::engine::OrderBook::Storage::Contiguous:
        return "contiguous";
    }
    return "baseline";
}

// QSL_BENCH_STORAGE lets the profiling workload A/B the order-book storage mode without rebuilding.
qsl::engine::OrderBook::Storage profile_storage_from_env() {
    const char *s = std::getenv("QSL_BENCH_STORAGE");
    const std::string_view v = (s != nullptr) ? s : "";
    if (v == "pooled") {
        return qsl::engine::OrderBook::Storage::Pooled;
    }
    if (v == "intrusive") {
        return qsl::engine::OrderBook::Storage::IntrusivePooled;
    }
    if (v == "contiguous") {
        return qsl::engine::OrderBook::Storage::Contiguous;
    }
    return qsl::engine::OrderBook::Storage::Baseline;
}

// Long-running, warm, deterministic profiling workload for `make flamegraph`. Drives a bounded
// steady-state order flow (add / cross / cancel / modify) through the matching engine for a
// wall-clock duration, so perf collects a dense sample set on a stable working set rather than the
// ~80ms one-shot benchmark suite. Wall-clock is fine here: this is the benchmark layer, never the
// deterministic engine path. The book stays ~W deep (cancel-oldest), keeping a pooled allocator
// warm so steady state issues no malloc/free.
void run_profile_workload(int argc, char **argv) {
    using namespace qsl;
    using core::Side;
    using core::TimeInForce;

    double seconds = 5.0;
    if (argc >= 3) {
        seconds = std::strtod(argv[2], nullptr);
    } else if (const char *e = std::getenv("QSL_BENCH_PROFILE_SECONDS")) {
        seconds = std::strtod(e, nullptr);
    }
    if (!(seconds > 0.0)) {
        seconds = 5.0;
    }

    const auto storage = profile_storage_from_env();
    engine::MatchingEngine eng{storage};
    const auto sym = eng.register_symbol("AAPL");

    constexpr std::size_t kRing = 512; // bounded resting depth
    std::vector<core::OrderId> ring;
    ring.reserve(kRing);
    std::size_t head = 0;

    // splitmix64 keeps the flow reproducible across runs/compilers without <random> overhead.
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&state] {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };

    core::OrderId id = 1;
    std::uint64_t ops = 0;
    const auto t0 = clock_type::now();
    const auto deadline = t0 + std::chrono::duration_cast<clock_type::duration>(
                                   std::chrono::duration<double>(seconds));
    while (clock_type::now() < deadline) {
        for (int k = 0; k < 4096; ++k) { // batch between clock reads
            const std::uint64_t r = next();
            const Side side = ((r & 1U) != 0U) ? Side::Buy : Side::Sell;
            const core::Price price = 100 + static_cast<core::Price>((r >> 1) % 64); // [100,164)
            const auto qty = 1 + static_cast<core::Quantity>((r >> 8) % 8);
            const core::OrderId oid = id++;
            g_sink += eng.new_limit(sym, oid, side, price, qty, TimeInForce::GTC).size();
            if (ring.size() < kRing) {
                ring.push_back(oid);
            } else {
                g_sink += eng.cancel(sym, ring[head]).size();
                ring[head] = oid;
                head = (head + 1) % kRing;
            }
            if ((r & 7U) == 0U && !ring.empty()) {
                const core::OrderId mid = ring[(head + (ring.size() / 2)) % ring.size()];
                g_sink += eng.modify(sym, mid, price, qty).size();
            }
            if ((r & 15U) == 0U) {
                g_sink +=
                    eng.new_market(sym, id++, ((r & 2U) != 0U) ? Side::Sell : Side::Buy, 3).size();
            }
            ++ops;
        }
    }
    const double secs = std::chrono::duration<double>(clock_type::now() - t0).count();
    std::printf("profile workload: storage=%s ops=%llu elapsed=%.3fs (%.0f ops/sec) resting~%zu\n",
                storage_name(storage), static_cast<unsigned long long>(ops), secs,
                static_cast<double>(ops) / secs, ring.size());
}

// Run a named benchmark subcommand (argv[1]); returns true if one matched and ran, so main exits.
bool run_subcommand(int argc, char **argv) {
    if (argc < 2) {
        return false;
    }
    const std::string command = argv[1];
    if (command == "diff") {
        run_diff_benchmarks();
    } else if (command == "profile") {
        run_profile_workload(argc, argv);
    } else if (command == "pool") {
        qsl::bench::run_order_pool_benchmarks();
    } else if (command == "storage") {
        qsl::bench::run_storage_benchmarks();
    } else if (command == "false-sharing") {
        qsl::bench::run_false_sharing_benchmarks();
    } else if (command == "recovery") {
        qsl::bench::run_recovery_benchmarks();
    } else {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (run_subcommand(argc, argv)) {
        return 0;
    }
    using namespace qsl;
    using core::OrderType;
    using core::Side;
    using core::TimeInForce;

    // 1. Order book add + modify + cancel latency (single symbol, bounded price band).
    {
        engine::OrderBook book;
        core::OrderId id = 1;
        latency("order_book add/mod/cancel", 200000, [&] {
            const core::OrderId oid = id++;
            const core::Price price = 100 + static_cast<core::Price>(oid % 10);
            g_sink += book.add_limit(oid, Side::Buy, price, 5, TimeInForce::GTC).size();
            g_sink += book.modify(oid, price, 3).size();
            g_sink += book.cancel(oid) ? 1U : 0U;
        });
    }

    // 2. Protocol NewOrder encode + decode round trip.
    {
        const protocol::NewOrder order{
            1, 0, 12345, 10, Side::Buy, OrderType::Limit, TimeInForce::GTC};
        latency("protocol encode+decode", 500000, [&] {
            const auto bytes = protocol::encode(order, 1);
            g_sink += protocol::decode_new_order(bytes).value.order_id;
        });
    }

    // 3. End-to-end in-process gateway session: a crossing buy that fills against deep resting
    //    liquidity each iteration (decode -> risk -> match -> encode Ack+Fill). Not an
    //    empty-book no-op: a real trade is produced on every call.
    {
        engine::MatchingEngine eng;
        eng.register_symbol("AAPL");
        gateway::OrderGateway gw{eng, gateway::RiskConfig{4'000'000, 1'000'000'000}};
        // Deep resting sell (5 units consumed per iter; 4M >> 200000 * 5).
        static_cast<void>(gw.new_limit(0, 999'999, Side::Sell, 100, 4'000'000, TimeInForce::GTC));
        gateway::Session session{gw};
        // A small GTC buy that fully fills, so it never rests and the frame stays reusable.
        const protocol::NewOrder buy{1, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC};
        const auto frame = protocol::encode(buy, 1);
        latency("gateway session (fill)", 200000,
                [&] { g_sink += session.on_bytes(frame).size(); });
    }

    // 4. Matching-engine throughput over a deterministic synthetic flow (seed 42).
    {
        const auto flow = replay::generate_flow(/*seed=*/42, /*symbols=*/4, /*orders=*/5000);
        throughput("matching engine flow", flow.size(), 20, [&] {
            engine::MatchingEngine eng;
            for (const auto &cmd : flow) {
                g_sink += replay::apply(eng, cmd).size();
            }
            g_sink += eng.last_seq();
        });
    }

    // 5. Replay throughput: rebuild engine state from a recorded command log.
    {
        const auto flow = replay::generate_flow(/*seed=*/42, /*symbols=*/4, /*orders=*/5000);
        std::vector<replay::LogRecord> records;
        std::uint64_t seq = 0;
        for (const auto &cmd : flow) {
            records.push_back({seq, RecordType::CommandRecord, seq, replay::encode_command(cmd)});
            ++seq;
        }
        throughput("replay command log", records.size(), 20, [&] {
            engine::MatchingEngine eng;
            g_sink += replay::replay(eng, records).size();
        });
    }

    return 0;
}

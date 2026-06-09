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
#include <string>
#include <vector>

namespace qsl::bench {
void run_order_pool_benchmarks();
void run_storage_benchmarks();
void run_false_sharing_benchmarks();
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

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "diff") {
        run_diff_benchmarks();
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "pool") {
        qsl::bench::run_order_pool_benchmarks();
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "storage") {
        qsl::bench::run_storage_benchmarks();
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "false-sharing") {
        qsl::bench::run_false_sharing_benchmarks();
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

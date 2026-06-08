#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/order_book.hpp"
#include "qsl/replay/recovery.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace qsl::bench {
namespace {

volatile std::uint64_t g_storage_sink = 0;

using clock_type = std::chrono::steady_clock;

void escape(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(p) : "memory");
#else
    (void)p;
#endif
}

void engine_flow(std::string_view name, engine::OrderBook::Storage storage, std::uint64_t seed,
                 core::SymbolId symbols, std::size_t orders, std::size_t reps) {
    const auto flow = replay::generate_flow(seed, symbols, orders);

    {
        engine::MatchingEngine warmup{storage};
        for (const auto &cmd : flow) {
            g_storage_sink += replay::apply(warmup, cmd).size();
        }
        g_storage_sink += warmup.last_seq();
        escape(&warmup);
    }

    const auto t0 = clock_type::now();
    std::size_t total_events = 0;
    std::size_t total_orders = 0;
    core::SeqNo last_seq = 0;
    for (std::size_t r = 0; r < reps; ++r) {
        engine::MatchingEngine eng{storage};
        for (const auto &cmd : flow) {
            const auto events = replay::apply(eng, cmd);
            total_events += events.size();
        }
        const auto snapshot = eng.snapshot();
        for (const auto &sym : snapshot.symbols) {
            total_orders += sym.order_count;
        }
        last_seq += eng.last_seq();
        escape(&eng);
    }
    const auto t1 = clock_type::now();

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double commands = static_cast<double>(flow.size() * reps);
    g_storage_sink += total_events + total_orders + last_seq;
    std::printf("%-34.*s %9zu cmds %8zu reps %10.1f ns/cmd %12.0f cmds/sec events=%zu resting=%zu "
                "last_seq=%llu\n",
                static_cast<int>(name.size()), name.data(), flow.size(), reps,
                (sec * 1e9) / commands, commands / sec, total_events, total_orders,
                static_cast<unsigned long long>(last_seq));
}

} // namespace

void run_storage_benchmarks() {
    constexpr std::uint64_t kSeed = 42;
    constexpr core::SymbolId kSymbols = 4;
    constexpr std::size_t kOrders = 5000;
    constexpr std::size_t kReps = 30;

    engine_flow("engine flow baseline storage", engine::OrderBook::Storage::Baseline, kSeed,
                kSymbols, kOrders, kReps);
    engine_flow("engine flow pooled pmr storage", engine::OrderBook::Storage::Pooled, kSeed,
                kSymbols, kOrders, kReps);
    engine_flow("engine flow intrusive pool storage", engine::OrderBook::Storage::IntrusivePooled,
                kSeed, kSymbols, kOrders, kReps);
}

} // namespace qsl::bench

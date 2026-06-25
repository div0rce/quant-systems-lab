// qsl-perfeval — a dedicated performance-evidence harness for the matching-engine hot path
// (order-book insertion + matching). It drives a steady-state deep-book order flow and reports
// orders/sec, per-order latency distribution (mean/p50/p99), and allocations/order. Run it under
// `perf stat` / `perf record` to attribute cycles/instructions/branch-misses and render
// flamegraphs.
//
// It is a SEPARATE binary from qsl-bench on purpose: it overrides global operator new/delete to
// count allocations, which must not perturb the committed qsl-bench numbers in results/latest.txt.
//
// Workload: each "order" is one new_limit submission (may match resting liquidity and rest its
// remainder); the book is held ~kRing deep by cancelling the oldest resting order each cycle. This
// matches the `qsl-bench profile` flamegraph workload's steady-state character. Baseline storage.

#include "qsl/engine/matching_engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {
// Allocations during the timed region only (the counter is sampled before and after the loop). The
// override below counts the primary std::operator new path used by the baseline pmr new_delete
// resource for order-book list/map nodes.
std::atomic<std::uint64_t> g_allocs{0};
} // namespace

void *operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void *operator new[](std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}

namespace {
using clk = std::chrono::steady_clock;
using namespace qsl;

constexpr std::size_t kRing = 512; // bounded resting depth
constexpr core::Price kBase = 100;
constexpr core::Price kBand = 64; // price band [100, 164)

std::uint64_t splitmix64(std::uint64_t &s) {
    s += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint32_t ns_between(clk::time_point a, clk::time_point b) {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Measured cost of two steady_clock reads around nothing, so the report can state how much of the
// per-order latency is timer overhead rather than engine work.
std::uint32_t timer_overhead_ns() {
    constexpr int kIters = 200000;
    std::uint64_t sum = 0;
    for (int i = 0; i < kIters; ++i) {
        const auto a = clk::now();
        const auto b = clk::now();
        sum += ns_between(a, b);
    }
    return static_cast<std::uint32_t>(sum / kIters);
}

struct PerfFlow {
    engine::MatchingEngine eng{}; // baseline storage
    core::SymbolId sym = eng.register_symbol("AAPL");
    std::vector<core::OrderId> ring{};
    std::size_t head = 0;
    core::OrderId id = 1;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    volatile std::uint64_t sink = 0;

    PerfFlow() { ring.reserve(kRing); }

    // Submit one limit order; returns its id so the caller can park it in the ring.
    core::OrderId submit() {
        const std::uint64_t r = splitmix64(state);
        const auto side = ((r & 1U) != 0U) ? core::Side::Buy : core::Side::Sell;
        const auto price = kBase + static_cast<core::Price>((r >> 1) % kBand);
        const auto qty = 1 + static_cast<core::Quantity>((r >> 8) % 8);
        const core::OrderId oid = id++;
        sink += eng.new_limit(sym, oid, side, price, qty, core::TimeInForce::GTC).size();
        return oid;
    }

    // Cancel the oldest resting order and park the new id in its slot, holding the book ~kRing
    // deep.
    void retire_oldest(core::OrderId oid) {
        sink += eng.cancel(sym, ring[head]).size();
        ring[head] = oid;
        head = (head + 1) % kRing;
    }

    void warmup() {
        while (ring.size() < kRing) {
            ring.push_back(submit());
        }
    }
};

struct Result {
    std::uint64_t orders = 0;
    double seconds = 0.0;
    double allocs_per_order = 0.0;
    bool has_latency = false;
    std::uint32_t mean_ns = 0, p50_ns = 0, p99_ns = 0, overhead_ns = 0;
};

void fill_latency_stats(std::vector<std::uint32_t> &lat, Result &res) {
    if (lat.empty()) {
        return;
    }
    std::sort(lat.begin(), lat.end());
    std::uint64_t sum = 0;
    for (const std::uint32_t v : lat) {
        sum += v;
    }
    res.mean_ns = static_cast<std::uint32_t>(sum / lat.size());
    res.p50_ns = lat[lat.size() / 2];
    res.p99_ns = lat[(lat.size() * 99) / 100];
    res.overhead_ns = timer_overhead_ns();
}

// Untimed run: cleanest for `perf stat`/`perf record` (no per-op clock overhead in the cycle
// count).
Result run_throughput(std::uint64_t orders) {
    PerfFlow flow;
    flow.warmup();
    const std::uint64_t a0 = g_allocs.load(std::memory_order_relaxed);
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < orders; ++i) {
        flow.retire_oldest(flow.submit());
    }
    const auto t1 = clk::now();
    Result res;
    res.orders = orders;
    res.seconds = std::chrono::duration<double>(t1 - t0).count();
    res.allocs_per_order = static_cast<double>(g_allocs.load(std::memory_order_relaxed) - a0) /
                           static_cast<double>(orders);
    return res;
}

// Timed run: records per-order new_limit latency. Absolute values include steady_clock overhead
// (reported separately); the before/after delta is the meaningful signal.
Result run_latency(std::uint64_t orders) {
    PerfFlow flow;
    flow.warmup();
    std::vector<std::uint32_t> lat;
    lat.reserve(orders);
    const std::uint64_t a0 = g_allocs.load(std::memory_order_relaxed);
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < orders; ++i) {
        const std::uint64_t r = splitmix64(flow.state);
        const auto side = ((r & 1U) != 0U) ? core::Side::Buy : core::Side::Sell;
        const auto price = kBase + static_cast<core::Price>((r >> 1) % kBand);
        const auto qty = 1 + static_cast<core::Quantity>((r >> 8) % 8);
        const core::OrderId oid = flow.id++;
        const auto a = clk::now();
        flow.sink +=
            flow.eng.new_limit(flow.sym, oid, side, price, qty, core::TimeInForce::GTC).size();
        const auto b = clk::now();
        lat.push_back(ns_between(a, b));
        flow.retire_oldest(oid);
    }
    const auto t1 = clk::now();
    Result res;
    res.orders = orders;
    res.seconds = std::chrono::duration<double>(t1 - t0).count();
    res.allocs_per_order = static_cast<double>(g_allocs.load(std::memory_order_relaxed) - a0) /
                           static_cast<double>(orders);
    res.has_latency = true;
    fill_latency_stats(lat, res);
    return res;
}

std::uint64_t parse_orders(int argc, char **argv, bool latency) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a != "--latency") {
            const std::uint64_t n = std::strtoull(a.c_str(), nullptr, 10);
            if (n > 0) {
                return n;
            }
        }
    }
    return latency ? 5'000'000ULL : 60'000'000ULL;
}
} // namespace

// qsl-perfeval [orders] [--latency]
//   default: untimed throughput + allocations/order (run under perf stat / perf record)
//   --latency: also record per-order latency mean/p50/p99
int main(int argc, char **argv) {
    bool latency = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--latency") {
            latency = true;
        }
    }
    const std::uint64_t orders = parse_orders(argc, argv, latency);
    const Result r = latency ? run_latency(orders) : run_throughput(orders);
    std::printf("perfeval: storage=baseline orders=%llu elapsed=%.3fs orders_per_sec=%.0f "
                "allocs_per_order=%.4f\n",
                static_cast<unsigned long long>(r.orders), r.seconds,
                static_cast<double>(r.orders) / r.seconds, r.allocs_per_order);
    if (r.has_latency) {
        std::printf("perfeval: latency_ns mean=%u p50=%u p99=%u (each incl ~%uns timer overhead)\n",
                    r.mean_ns, r.p50_ns, r.p99_ns, r.overhead_ns);
    }
    return 0;
}

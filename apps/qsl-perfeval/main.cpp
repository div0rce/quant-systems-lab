// qsl-perfeval, a dedicated performance-evidence harness for the matching-engine hot path
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
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

// Allocation counting replaces every global operator new variant so it can count ALL allocations
// (plain AND over-aligned, the latter used by the order book's alignas types). Counting and the
// pure-performance run are mutually exclusive on purpose: intercepting the aligned operators adds a
// little work per aligned allocation, which would perturb the cycle/throughput numbers for an
// allocation-heavy build. So counting is compile-time opt-in (QSL_PERFEVAL_COUNT_ALLOCS); the
// default build leaves the system allocator untouched and reports allocs/order as "n/a". Measure
// allocations and performance in separate builds (see PERFORMANCE.md methodology).
#ifdef QSL_PERFEVAL_COUNT_ALLOCS
namespace {
std::atomic<std::uint64_t> g_allocs{0};
constexpr bool kCountingAllocs = true;
std::uint64_t allocs_now() {
    return g_allocs.load(std::memory_order_relaxed);
}
void *aligned_new(std::size_t n, std::align_val_t a) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    // aligned_alloc (what libstdc++'s default aligned new uses) requires size to be a multiple of
    // the alignment.
    const std::size_t align = std::max(sizeof(void *), static_cast<std::size_t>(a));
    const std::size_t size = (n + align - 1) & ~(align - 1);
    if (void *p = std::aligned_alloc(align, size)) {
        return p;
    }
    throw std::bad_alloc();
}
} // namespace

void *operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void *operator new(std::size_t n, std::align_val_t a) {
    return aligned_new(n, a);
}
void *operator new[](std::size_t n, std::align_val_t a) {
    return aligned_new(n, a);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void *p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
#else
namespace {
constexpr bool kCountingAllocs = false;
std::uint64_t allocs_now() {
    return 0;
}
} // namespace
#endif

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

    // Submit one limit order; returns its id. It may match resting liquidity and rest its
    // remainder, or fully fill (marketable) and never rest.
    core::OrderId submit() {
        const std::uint64_t r = splitmix64(state);
        const auto side = ((r & 1U) != 0U) ? core::Side::Buy : core::Side::Sell;
        const auto price = kBase + static_cast<core::Price>((r >> 1) % kBand);
        const auto qty = 1 + static_cast<core::Quantity>((r >> 8) % 8);
        const core::OrderId oid = id++;
        sink += eng.new_limit(sym, oid, side, price, qty, core::TimeInForce::GTC).size();
        return oid;
    }

    // Track only orders that actually rested, holding the book ~kRing deep: a fully-filled order is
    // ignored, a resting order is parked, and once the ring is full the oldest resting order is
    // cancelled. Checking eng.contains keeps depth steady instead of drifting with the match rate
    // (and never cancels an id that never rested).
    void track(core::OrderId oid) {
        if (!eng.contains(sym, oid)) {
            return;
        }
        if (ring.size() < kRing) {
            ring.push_back(oid);
            return;
        }
        sink += eng.cancel(sym, ring[head]).size();
        ring[head] = oid;
        head = (head + 1) % kRing;
    }

    void warmup() {
        while (ring.size() < kRing) {
            track(submit());
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
    res.p50_ns = lat[(lat.size() - 1) / 2];
    res.p99_ns = lat[((lat.size() - 1) * 99) / 100];
    res.overhead_ns = timer_overhead_ns();
}

// Untimed run: cleanest for `perf stat`/`perf record` (no per-op clock overhead in the cycle
// count).
Result run_throughput(std::uint64_t orders) {
    PerfFlow flow;
    flow.warmup();
    const std::uint64_t a0 = allocs_now();
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < orders; ++i) {
        flow.track(flow.submit());
    }
    const auto t1 = clk::now();
    Result res;
    res.orders = orders;
    res.seconds = std::chrono::duration<double>(t1 - t0).count();
    res.allocs_per_order = static_cast<double>(allocs_now() - a0) / static_cast<double>(orders);
    return res;
}

// Timed run: records per-order new_limit latency. Absolute values include steady_clock overhead
// (reported separately); the before/after delta is the meaningful signal.
Result run_latency(std::uint64_t orders) {
    PerfFlow flow;
    flow.warmup();
    std::vector<std::uint32_t> lat;
    lat.reserve(orders);
    const std::uint64_t a0 = allocs_now();
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
        flow.track(oid);
    }
    const auto t1 = clk::now();
    Result res;
    res.orders = orders;
    res.seconds = std::chrono::duration<double>(t1 - t0).count();
    res.allocs_per_order = static_cast<double>(allocs_now() - a0) / static_cast<double>(orders);
    res.has_latency = true;
    fill_latency_stats(lat, res);
    return res;
}

// Parse a whole token as a positive order count. Rejects partial parses ("123abc"), trailing junk,
// and negative-looking input ("-1") that std::strtoull would wrap to a huge value and feed straight
// into reserve()/the loop bound.
std::optional<std::uint64_t> parse_orders_arg(std::string_view s) {
    std::uint64_t v = 0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end || v == 0) {
        return std::nullopt;
    }
    return v;
}
} // namespace

// qsl-perfeval [orders>0] [--latency]
//   default: untimed throughput + allocations/order (run under perf stat / perf record)
//   --latency: also record per-order latency mean/p50/p99
int main(int argc, char **argv) {
    bool latency = false;
    std::optional<std::uint64_t> orders;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--latency") {
            latency = true;
            continue;
        }
        const auto parsed = parse_orders_arg(a);
        if (!parsed) {
            std::fprintf(stderr, "usage: qsl-perfeval [orders>0] [--latency]\n");
            return 2;
        }
        orders = *parsed;
    }
    const std::uint64_t count = orders.value_or(latency ? 5'000'000ULL : 60'000'000ULL);
    const Result r = latency ? run_latency(count) : run_throughput(count);
    char allocs_buf[24];
    if constexpr (kCountingAllocs) {
        std::snprintf(allocs_buf, sizeof(allocs_buf), "%.4f", r.allocs_per_order);
    } else {
        std::snprintf(allocs_buf, sizeof(allocs_buf), "n/a");
    }
    std::printf("perfeval: storage=baseline orders=%llu elapsed=%.3fs orders_per_sec=%.0f "
                "allocs_per_order=%s\n",
                static_cast<unsigned long long>(r.orders), r.seconds,
                static_cast<double>(r.orders) / r.seconds, allocs_buf);
    if (r.has_latency) {
        std::printf("perfeval: latency_ns mean=%u p50=%u p99=%u (each incl ~%uns timer overhead)\n",
                    r.mean_ns, r.p50_ns, r.p99_ns, r.overhead_ns);
    }
    return 0;
}

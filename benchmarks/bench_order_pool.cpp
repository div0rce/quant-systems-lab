#include "qsl/core/types.hpp"
#include "qsl/engine/order.hpp"
#include "qsl/memory/order_pool.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace qsl::bench {
namespace {

volatile std::uint64_t g_pool_sink = 0;

using clock_type = std::chrono::steady_clock;

void escape(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(p) : "memory");
#else
    (void)p;
#endif
}

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

} // namespace

void run_order_pool_benchmarks() {
    using core::Side;
    constexpr std::size_t kIters = 500000;

    latency("order new/delete", kIters, [] {
        void *raw = ::operator new(sizeof(engine::Order));
        escape(raw);
        auto *order = new (raw) engine::Order{1, Side::Buy, 100, 5};
        escape(order);
        g_pool_sink += order->id;
        order->~Order();
        ::operator delete(raw);
    });

    {
        memory::OrderPool<1024> pool;
        core::OrderId id = 1;
        latency("order pool acquire/release", kIters, [&] {
            engine::Order *order = pool.try_acquire(id++, Side::Buy, 100, 5);
            if (order == nullptr) {
                std::abort();
            }
            escape(order);
            g_pool_sink += order->id;
            if (!pool.release(order)) {
                std::abort();
            }
        });
    }

    {
        memory::OrderPool<1024> pool;
        std::array<engine::Order *, 1024> orders{};
        latency("order pool burst cycle", 2000, [&] {
            for (std::size_t i = 0; i < orders.size(); ++i) {
                orders[i] = pool.try_acquire(static_cast<core::OrderId>(i + 1), Side::Sell, 101, 3);
                if (orders[i] == nullptr) {
                    std::abort();
                }
                escape(orders[i]);
                g_pool_sink += orders[i]->quantity;
            }
            for (auto *order : orders) {
                if (!pool.release(order)) {
                    std::abort();
                }
            }
        });
    }
}

} // namespace qsl::bench

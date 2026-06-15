#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/order.hpp"
#include "qsl/engine/order_book.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/recovery.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace qsl::bench {
namespace {

volatile std::uint64_t g_storage_sink = 0;

using clock_type = std::chrono::steady_clock;

struct ActiveOrder {
    core::SymbolId symbol;
    engine::Order order;
};

struct LimitSpec {
    core::SymbolId symbol;
    core::Side side;
    core::Price price;
    core::Quantity quantity;
    core::TimeInForce tif = core::TimeInForce::GTC;
};

struct Workload {
    std::string_view name;
    std::uint64_t seed;
    std::string_view purpose;
    std::vector<replay::Command> commands;
    core::SymbolId symbols;
    std::size_t top_probe_interval = 0;
};

struct WorkloadShape {
    std::size_t commands = 0;
    std::size_t new_limits = 0;
    std::size_t accepted = 0;
    std::size_t events = 0;
    std::size_t trades = 0;
    std::size_t cancel_cmds = 0;
    std::size_t modify_cmds = 0;
    std::size_t market_orders = 0;
    std::size_t ioc_orders = 0;
    std::size_t canceled_events = 0;
    std::size_t modified_events = 0;
    std::size_t max_bid_levels = 0;
    std::size_t max_ask_levels = 0;
    std::size_t max_active_levels = 0;
    std::size_t max_resting_orders = 0;
    std::size_t final_resting_orders = 0;
    std::uint64_t bid_level_samples = 0;
    std::uint64_t ask_level_samples = 0;
    std::size_t snapshot_samples = 0;
    std::size_t top_probe_calls = 0;
    std::vector<core::Price> submitted_prices;
};

struct RunSummary {
    std::size_t events = 0;
    std::size_t resting_orders = 0;
    core::SeqNo last_seq = 0;
    std::size_t top_probe_calls = 0;
};

struct Timing {
    double median_ns = 0.0;
    double min_ns = 0.0;
    double max_ns = 0.0;
    RunSummary summary;
    std::size_t timed_commands = 0;
};

void escape(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(p) : "memory");
#else
    (void)p;
#endif
}

[[nodiscard]] core::Quantity reduced_quantity(core::Quantity quantity) noexcept {
    return quantity > 1 ? static_cast<core::Quantity>(quantity - 1) : core::Quantity{1};
}

class WorkloadBuilder {
  public:
    explicit WorkloadBuilder(core::SymbolId symbols) : symbols_(symbols) {
        for (core::SymbolId symbol = 0; symbol < symbols_; ++symbol) {
            push(replay::RegisterSymbol{"S" + std::to_string(symbol)});
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] bool has_active() const noexcept { return !active_.empty(); }

    void add_limit(LimitSpec spec) {
        push(replay::NewLimit{spec.symbol, next_id_++, spec.side, spec.price, spec.quantity,
                              spec.tif});
    }

    void add_market(core::SymbolId symbol, core::Side side, core::Quantity quantity) {
        push(replay::NewMarket{symbol, next_id_++, side, quantity});
    }

    void cancel_active(std::uint64_t salt) {
        const auto target = pick_active(salt);
        if (target) {
            push(replay::Cancel{target->symbol, target->order.id});
        }
    }

    void duplicate_active(std::uint64_t salt) {
        const auto target = pick_active(salt);
        if (target) {
            push(replay::NewLimit{target->symbol, target->order.id, target->order.side,
                                  target->order.price, target->order.quantity,
                                  core::TimeInForce::GTC});
        }
    }

    void reduce_active(std::uint64_t salt) {
        const auto target = pick_active(salt);
        if (target) {
            push(replay::Modify{target->symbol, target->order.id, target->order.price,
                                reduced_quantity(target->order.quantity)});
        }
    }

    [[nodiscard]] Workload finish(std::string_view name, std::uint64_t seed,
                                  std::string_view purpose, std::size_t top_probe_interval = 0) {
        return Workload{name, seed, purpose, std::move(commands_), symbols_, top_probe_interval};
    }

  private:
    [[nodiscard]] std::optional<ActiveOrder> pick_active(std::uint64_t salt) const {
        if (active_.empty()) {
            return std::nullopt;
        }
        return active_[static_cast<std::size_t>(salt % active_.size())];
    }

    void push(replay::Command command) {
        commands_.push_back(std::move(command));
        static_cast<void>(replay::apply(model_, commands_.back()));
        refresh_active();
    }

    void refresh_active() {
        active_.clear();
        for (core::SymbolId symbol = 0; symbol < symbols_; ++symbol) {
            for (const auto &order : model_.resting_orders(symbol)) {
                active_.push_back(ActiveOrder{symbol, order});
            }
        }
    }

    core::SymbolId symbols_;
    engine::MatchingEngine model_;
    std::vector<replay::Command> commands_;
    std::vector<ActiveOrder> active_;
    core::OrderId next_id_ = 1;
};

void add_price(WorkloadShape &shape, core::Price price) {
    shape.submitted_prices.push_back(price);
}

void observe_command(const replay::Command &command, WorkloadShape &shape) {
    ++shape.commands;
    if (const auto *limit = std::get_if<replay::NewLimit>(&command)) {
        ++shape.new_limits;
        shape.ioc_orders += limit->tif == core::TimeInForce::IOC ? 1U : 0U;
        add_price(shape, limit->price);
        return;
    }
    if (std::holds_alternative<replay::NewMarket>(command)) {
        ++shape.market_orders;
        return;
    }
    if (std::holds_alternative<replay::Cancel>(command)) {
        ++shape.cancel_cmds;
        return;
    }
    if (const auto *modify = std::get_if<replay::Modify>(&command)) {
        ++shape.modify_cmds;
        add_price(shape, modify->price);
    }
}

void observe_events(const std::vector<engine::EngineEvent> &events, WorkloadShape &shape) {
    shape.events += events.size();
    for (const auto &event : events) {
        if (std::holds_alternative<engine::OrderAccepted>(event)) {
            ++shape.accepted;
        } else if (std::holds_alternative<engine::TradeEvent>(event)) {
            ++shape.trades;
        } else if (std::holds_alternative<engine::OrderCanceled>(event)) {
            ++shape.canceled_events;
        } else if (std::holds_alternative<engine::OrderModified>(event)) {
            ++shape.modified_events;
        }
    }
}

void observe_snapshot(const engine::EngineSnapshot &snapshot, WorkloadShape &shape) {
    std::size_t bid_levels = 0;
    std::size_t ask_levels = 0;
    std::size_t resting_orders = 0;
    for (const auto &symbol : snapshot.symbols) {
        bid_levels += symbol.bids.size();
        ask_levels += symbol.asks.size();
        resting_orders += symbol.order_count;
    }
    shape.max_bid_levels = std::max(shape.max_bid_levels, bid_levels);
    shape.max_ask_levels = std::max(shape.max_ask_levels, ask_levels);
    shape.max_active_levels = std::max(shape.max_active_levels, bid_levels + ask_levels);
    shape.max_resting_orders = std::max(shape.max_resting_orders, resting_orders);
    shape.final_resting_orders = resting_orders;
    shape.bid_level_samples += bid_levels;
    shape.ask_level_samples += ask_levels;
    ++shape.snapshot_samples;
}

[[nodiscard]] WorkloadShape characterize(const Workload &workload) {
    WorkloadShape shape;
    engine::MatchingEngine engine;
    for (const auto &command : workload.commands) {
        observe_command(command, shape);
        const auto events = replay::apply(engine, command);
        observe_events(events, shape);
        observe_snapshot(engine.snapshot(), shape);
    }
    shape.top_probe_calls =
        workload.top_probe_interval == 0
            ? 0
            : (shape.commands / workload.top_probe_interval) * workload.symbols * 2U;
    return shape;
}

[[nodiscard]] std::size_t price_domain_width(const WorkloadShape &shape) {
    if (shape.submitted_prices.empty()) {
        return 0;
    }
    const auto [min_it, max_it] =
        std::minmax_element(shape.submitted_prices.begin(), shape.submitted_prices.end());
    return static_cast<std::size_t>(*max_it - *min_it + 1);
}

[[nodiscard]] double price_density(const WorkloadShape &shape, core::SymbolId symbols) {
    const std::size_t width = price_domain_width(shape);
    if (width == 0) {
        return 0.0;
    }
    const auto slots = static_cast<double>(width) * static_cast<double>(symbols) * 2.0;
    return static_cast<double>(shape.max_active_levels) / slots;
}

[[nodiscard]] double average_levels(std::uint64_t samples, std::size_t count) {
    if (count == 0) {
        return 0.0;
    }
    return static_cast<double>(samples) / static_cast<double>(count);
}

[[nodiscard]] bool should_probe(const Workload &workload, std::size_t command_index) noexcept {
    return workload.top_probe_interval != 0 &&
           ((command_index + 1) % workload.top_probe_interval) == 0;
}

[[nodiscard]] std::uint64_t probe_top_of_book(const engine::MatchingEngine &engine,
                                              core::SymbolId symbols) {
    std::uint64_t checksum = 0;
    for (core::SymbolId symbol = 0; symbol < symbols; ++symbol) {
        if (const auto bid = engine.best_bid(symbol)) {
            checksum += static_cast<std::uint64_t>(*bid);
        }
        if (const auto ask = engine.best_ask(symbol)) {
            checksum += static_cast<std::uint64_t>(*ask);
        }
    }
    return checksum;
}

// Apply the leading RegisterSymbol prefix. Registration eagerly constructs each per-symbol
// OrderBook, which for the pooled modes runs the fixed-capacity free-list initialization. This is
// per-run setup, not command-path work, so callers run it outside the timed interval. Returns the
// index of the first non-registration (trading) command.
[[nodiscard]] std::size_t apply_registration(engine::MatchingEngine &engine,
                                             const Workload &workload) {
    std::size_t index = 0;
    while (index < workload.commands.size() &&
           std::holds_alternative<replay::RegisterSymbol>(workload.commands[index])) {
        static_cast<void>(replay::apply(engine, workload.commands[index]));
        ++index;
    }
    return index;
}

// Apply the trading commands [start, end) -- the timed command path.
[[nodiscard]] RunSummary apply_trading(engine::MatchingEngine &engine, const Workload &workload,
                                       std::size_t start) {
    RunSummary summary;
    for (std::size_t i = start; i < workload.commands.size(); ++i) {
        const auto events = replay::apply(engine, workload.commands[i]);
        summary.events += events.size();
        if (should_probe(workload, i)) {
            g_storage_sink += probe_top_of_book(engine, workload.symbols);
            summary.top_probe_calls += workload.symbols * 2U;
        }
    }
    return summary;
}

// End-of-run readout (snapshot accounting + dead-code-elimination sink). Not command-path work,
// so it runs outside the timed interval.
void finalize_run(engine::MatchingEngine &engine, RunSummary &summary) {
    const auto snapshot = engine.snapshot();
    for (const auto &symbol : snapshot.symbols) {
        summary.resting_orders += symbol.order_count;
    }
    summary.last_seq = engine.last_seq();
    g_storage_sink +=
        summary.events + summary.resting_orders + summary.last_seq + summary.top_probe_calls;
    escape(&engine);
}

[[nodiscard]] RunSummary run_once(engine::OrderBook::Storage storage, const Workload &workload) {
    engine::MatchingEngine engine{storage};
    const std::size_t start = apply_registration(engine, workload);
    RunSummary summary = apply_trading(engine, workload, start);
    finalize_run(engine, summary);
    return summary;
}

[[nodiscard]] Timing time_storage(engine::OrderBook::Storage storage, const Workload &workload,
                                  std::size_t reps) {
    static_cast<void>(run_once(storage, workload)); // warmup
    std::vector<double> samples;
    samples.reserve(reps);
    RunSummary last;
    std::size_t timed_commands = 0;
    for (std::size_t r = 0; r < reps; ++r) {
        engine::MatchingEngine engine{storage};
        // Per-run setup (engine construction + symbol registration, including the pooled modes'
        // free-list initialization) runs untimed so each sample measures only the command path.
        const std::size_t start = apply_registration(engine, workload);
        timed_commands = workload.commands.size() - start;
        const auto t0 = clock_type::now();
        RunSummary summary = apply_trading(engine, workload, start);
        const auto t1 = clock_type::now();
        // The end-of-run snapshot readout is bookkeeping, not command work; keep it untimed.
        finalize_run(engine, summary);
        last = summary;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns / static_cast<double>(timed_commands));
    }
    std::sort(samples.begin(), samples.end());
    return Timing{samples[samples.size() / 2], samples.front(), samples.back(), last,
                  timed_commands};
}

void print_shape(const Workload &workload, const WorkloadShape &shape) {
    std::printf("\nWorkload: %.*s (seed=%llu)\n", static_cast<int>(workload.name.size()),
                workload.name.data(), static_cast<unsigned long long>(workload.seed));
    std::printf("Purpose:  %.*s\n", static_cast<int>(workload.purpose.size()),
                workload.purpose.data());
    std::printf("Shape:    commands=%zu events=%zu accepted=%zu trades=%zu cancel_cmds=%zu "
                "modify_cmds=%zu market_orders=%zu ioc_orders=%zu canceled_events=%zu "
                "modified_events=%zu final_resting=%zu max_resting=%zu max_bid_levels=%zu "
                "max_ask_levels=%zu avg_bid_levels=%.1f avg_ask_levels=%.1f max_active_levels=%zu "
                "price_width=%zu price_density=%.3f top_probe_calls=%zu\n",
                shape.commands, shape.events, shape.accepted, shape.trades, shape.cancel_cmds,
                shape.modify_cmds, shape.market_orders, shape.ioc_orders, shape.canceled_events,
                shape.modified_events, shape.final_resting_orders, shape.max_resting_orders,
                shape.max_bid_levels, shape.max_ask_levels,
                average_levels(shape.bid_level_samples, shape.snapshot_samples),
                average_levels(shape.ask_level_samples, shape.snapshot_samples),
                shape.max_active_levels, price_domain_width(shape),
                price_density(shape, workload.symbols), shape.top_probe_calls);
}

void print_timing(const Workload &workload, std::string_view mode, const Timing &timing,
                  std::size_t reps) {
    const double commands_per_sec = 1e9 / timing.median_ns;
    std::printf(
        "%-24.*s %-18.*s %7zu cmds %5zu reps median %8.1f ns/cmd min %8.1f "
        "max %8.1f %12.0f cmds/sec events/run=%zu resting/run=%zu last_seq/run=%llu "
        "probes/run=%zu\n",
        static_cast<int>(workload.name.size()), workload.name.data(), static_cast<int>(mode.size()),
        mode.data(), timing.timed_commands, reps, timing.median_ns, timing.min_ns, timing.max_ns,
        commands_per_sec, timing.summary.events, timing.summary.resting_orders,
        static_cast<unsigned long long>(timing.summary.last_seq), timing.summary.top_probe_calls);
}

void benchmark_workload(const Workload &workload, std::size_t reps) {
    print_shape(workload, characterize(workload));
    print_timing(workload, "baseline",
                 time_storage(engine::OrderBook::Storage::Baseline, workload, reps), reps);
    print_timing(workload, "pooled pmr",
                 time_storage(engine::OrderBook::Storage::Pooled, workload, reps), reps);
    print_timing(workload, "intrusive pool",
                 time_storage(engine::OrderBook::Storage::IntrusivePooled, workload, reps), reps);
    print_timing(workload, "contiguous",
                 time_storage(engine::OrderBook::Storage::Contiguous, workload, reps), reps);
}

[[nodiscard]] Workload general_generated_flow() {
    return Workload{"general generated flow",
                    42,
                    "Existing deterministic generated engine flow; mixed insert, match, cancel, "
                    "modify, IOC, and market activity.",
                    replay::generate_flow(/*seed=*/42, /*symbols=*/4, /*orders=*/5000),
                    /*symbols=*/4,
                    /*top_probe_interval=*/0};
}

void seed_dense_bounded_book(WorkloadBuilder &builder) {
    for (core::Price price = 90; price <= 113; ++price) {
        builder.add_limit({0, core::Side::Buy, price, 8});
    }
    for (core::Price price = 115; price <= 138; ++price) {
        builder.add_limit({0, core::Side::Sell, price, 8});
    }
    for (core::Price price = 190; price <= 205; ++price) {
        builder.add_limit({1, core::Side::Buy, price, 6});
        builder.add_limit({1, core::Side::Sell, price + 20, 6});
    }
}

void append_dense_bounded_step(WorkloadBuilder &builder, std::uint64_t i) {
    const auto phase = i % 10;
    if (phase < 3) {
        builder.add_limit({0, core::Side::Buy, 98 + static_cast<core::Price>(i % 16), 4});
        return;
    }
    if (phase < 5) {
        builder.add_limit({0, core::Side::Sell, 115 + static_cast<core::Price>(i % 16), 4});
        return;
    }
    if (phase == 5) {
        builder.add_market(0, core::Side::Buy, 1);
        return;
    }
    if (phase == 6) {
        builder.add_market(0, core::Side::Sell, 1);
        return;
    }
    if (phase == 7) {
        builder.add_limit({0, core::Side::Buy, 115, 2, core::TimeInForce::IOC});
        return;
    }
    if (phase == 8) {
        builder.reduce_active(i);
        return;
    }
    builder.duplicate_active(i);
}

[[nodiscard]] Workload dense_bounded_flow() {
    WorkloadBuilder builder{/*symbols=*/2};
    seed_dense_bounded_book(builder);
    std::uint64_t i = 0;
    while (builder.size() < 5004) {
        append_dense_bounded_step(builder, i);
        ++i;
    }
    return builder.finish(
        "dense bounded flow", 4702,
        "Small bounded price domain with many live levels, repeated same-price operations, and "
        "top-of-book probes after every command.",
        /*top_probe_interval=*/1);
}

using PriceSet = std::array<core::Price, 4>;

void seed_sparse_wide_book(WorkloadBuilder &builder, const PriceSet &bid_prices,
                           const PriceSet &ask_prices) {
    for (core::SymbolId symbol = 0; symbol < 4; ++symbol) {
        for (const auto price : bid_prices) {
            builder.add_limit({symbol, core::Side::Buy, price, 3});
        }
        for (const auto price : ask_prices) {
            builder.add_limit({symbol, core::Side::Sell, price, 3});
        }
    }
}

void append_sparse_wide_step(WorkloadBuilder &builder, std::uint64_t i, const PriceSet &bid_prices,
                             const PriceSet &ask_prices) {
    const auto symbol = static_cast<core::SymbolId>(i % 4);
    const auto phase = i % 6;
    if (phase < 2) {
        builder.add_limit({symbol, core::Side::Buy, bid_prices[i % bid_prices.size()], 2});
        return;
    }
    if (phase < 4) {
        builder.add_limit({symbol, core::Side::Sell, ask_prices[i % ask_prices.size()], 2});
        return;
    }
    if (phase == 4) {
        builder.cancel_active(i);
        return;
    }
    builder.reduce_active(i);
}

[[nodiscard]] Workload sparse_wide_flow() {
    WorkloadBuilder builder{/*symbols=*/4};
    constexpr PriceSet bid_prices{16, 128, 384, 640};
    constexpr PriceSet ask_prices{720, 840, 960, 1000};
    seed_sparse_wide_book(builder, bid_prices, ask_prices);
    std::uint64_t i = 0;
    while (builder.size() < 5004) {
        append_sparse_wide_step(builder, i, bid_prices, ask_prices);
        ++i;
    }
    return builder.finish("sparse wide flow", 4703,
                          "Wide in-band price domain with few active levels and many gaps.",
                          /*top_probe_interval=*/0);
}

void seed_cancel_modify_book(WorkloadBuilder &builder) {
    for (core::SymbolId symbol = 0; symbol < 3; ++symbol) {
        for (core::Price price = 90; price < 100; ++price) {
            builder.add_limit({symbol, core::Side::Buy, price, 5});
            builder.add_limit({symbol, core::Side::Sell, price + 20, 5});
        }
    }
}

void add_cancel_modify_replenishment(WorkloadBuilder &builder, std::mt19937_64 &rng) {
    const auto symbol = static_cast<core::SymbolId>(rng() % 3);
    const bool buy = (rng() % 2) == 0;
    const core::Price price = buy ? 90 + static_cast<core::Price>(rng() % 10)
                                  : 110 + static_cast<core::Price>(rng() % 10);
    builder.add_limit({symbol, buy ? core::Side::Buy : core::Side::Sell, price, 5});
}

void append_cancel_modify_step(WorkloadBuilder &builder, std::mt19937_64 &rng) {
    if (!builder.has_active()) {
        builder.add_limit({0, core::Side::Buy, 95, 5});
        return;
    }
    const auto pick = static_cast<int>(rng() % 100);
    if (pick < 15) {
        add_cancel_modify_replenishment(builder, rng);
        return;
    }
    if (pick < 55) {
        builder.cancel_active(rng());
        return;
    }
    if (pick < 95) {
        builder.reduce_active(rng());
        return;
    }
    builder.duplicate_active(rng());
}

[[nodiscard]] Workload cancel_modify_heavy_flow() {
    WorkloadBuilder builder{/*symbols=*/3};
    seed_cancel_modify_book(builder);
    std::mt19937_64 rng{4704};
    while (builder.size() < 5004) {
        append_cancel_modify_step(builder, rng);
    }
    return builder.finish("cancel/modify-heavy flow", 4704,
                          "Locator-heavy workload with frequent active cancels, in-place "
                          "modifies, replenishment, and duplicate active ids.",
                          /*top_probe_interval=*/0);
}

void seed_match_traversal_book(WorkloadBuilder &builder) {
    for (core::Price price = 80; price < 100; ++price) {
        builder.add_limit({0, core::Side::Buy, price, 1});
    }
    for (core::Price price = 100; price < 140; ++price) {
        builder.add_limit({0, core::Side::Sell, price, 1});
    }
}

void append_match_traversal_step(WorkloadBuilder &builder, std::uint64_t i) {
    const auto phase = i % 20;
    if (phase < 8) {
        builder.add_limit({0, core::Side::Sell, 100 + static_cast<core::Price>(i % 40), 1});
        return;
    }
    if (phase < 16) {
        builder.add_limit({0, core::Side::Buy, 99 - static_cast<core::Price>(i % 40), 1});
        return;
    }
    if (phase == 16) {
        builder.add_market(0, core::Side::Buy, 12);
        return;
    }
    if (phase == 17) {
        builder.add_market(0, core::Side::Sell, 12);
        return;
    }
    if (phase == 18) {
        builder.add_limit({0, core::Side::Buy, 140, 8, core::TimeInForce::IOC});
        return;
    }
    builder.add_limit({0, core::Side::Sell, 60, 8, core::TimeInForce::IOC});
}

[[nodiscard]] Workload match_traversal_heavy_flow() {
    WorkloadBuilder builder{/*symbols=*/1};
    seed_match_traversal_book(builder);
    std::uint64_t i = 0;
    while (builder.size() < 5004) {
        append_match_traversal_step(builder, i);
        ++i;
    }
    return builder.finish("match/traversal-heavy flow", 4705,
                          "Many small maker orders per level sweep, stressing level traversal and "
                          "best-price maintenance.",
                          /*top_probe_interval=*/0);
}

} // namespace

void run_storage_benchmarks() {
    constexpr std::size_t kReps = 30;
    const std::vector<Workload> workloads{general_generated_flow(), dense_bounded_flow(),
                                          sparse_wide_flow(), cancel_modify_heavy_flow(),
                                          match_traversal_heavy_flow()};
    for (const auto &workload : workloads) {
        benchmark_workload(workload, kReps);
    }
}

} // namespace qsl::bench

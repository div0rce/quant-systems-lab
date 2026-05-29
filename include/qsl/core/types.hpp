#pragma once

#include <cstdint>

namespace qsl::core {

// Compact integer domain identifiers. Prices are integer ticks; never floating point.
using SymbolId = std::uint32_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;
// Aggregate of many per-order Quantity values (e.g. total resting size at a price
// level). Wider than Quantity so level/snapshot totals never wrap.
using QuantityTotal = std::uint64_t;
using OrderId = std::uint64_t;
using SeqNo = std::uint64_t;

// Logical (deterministic) time used inside replayable engine paths. Not wall-clock.
using Timestamp = std::uint64_t;

// Prices are expressed in integer ticks: e.g. $123.45 at tick scale 100 => 12345.
inline constexpr std::int64_t kTickScale = 100;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
enum class TimeInForce : std::uint8_t { GTC = 0, IOC = 1 };

[[nodiscard]] constexpr bool is_valid(Side s) noexcept {
    return s == Side::Buy || s == Side::Sell;
}

[[nodiscard]] constexpr bool is_valid(OrderType t) noexcept {
    return t == OrderType::Limit || t == OrderType::Market;
}

[[nodiscard]] constexpr bool is_valid(TimeInForce t) noexcept {
    return t == TimeInForce::GTC || t == TimeInForce::IOC;
}

[[nodiscard]] constexpr const char *to_string(Side s) noexcept {
    switch (s) {
    case Side::Buy:
        return "Buy";
    case Side::Sell:
        return "Sell";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char *to_string(OrderType t) noexcept {
    switch (t) {
    case OrderType::Limit:
        return "Limit";
    case OrderType::Market:
        return "Market";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char *to_string(TimeInForce t) noexcept {
    switch (t) {
    case TimeInForce::GTC:
        return "GTC";
    case TimeInForce::IOC:
        return "IOC";
    }
    return "Unknown";
}

/// Smoke-test function to verify build.
[[nodiscard]] auto version() noexcept -> const char *;

} // namespace qsl::core

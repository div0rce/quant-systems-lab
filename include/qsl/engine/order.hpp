#pragma once

#include "qsl/core/types.hpp"

namespace qsl::engine {

using core::OrderId;
using core::Price;
using core::Quantity;
using core::QuantityTotal;
using core::Side;

// A resting order in the book. Time priority is its position within a price level.
struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity; // remaining
    bool operator==(const Order &) const = default;
};

// A match between an aggressing (taker) order and a resting (maker) order.
// By price-time priority the trade executes at the resting maker's price.
struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Price price;
    Quantity quantity;
};

// Aggregate resting quantity at one price level (used for snapshots/replay comparison).
struct LevelView {
    Price price;
    QuantityTotal quantity;
    bool operator==(const LevelView &) const = default;
};

} // namespace qsl::engine

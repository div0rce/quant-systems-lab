#pragma once

#include "qsl/core/types.hpp"

#include <variant>

namespace qsl::engine {

using core::OrderId;
using core::Price;
using core::Quantity;
using core::SeqNo;
using core::SymbolId;

// Engine output events. Each carries the sequence number assigned when it was emitted.
// OrderRejected (M5 risk) and BookUpdate (M6 market data) are added by later milestones.

struct OrderAccepted {
    SeqNo seq;
    SymbolId symbol;
    OrderId order_id;
    bool operator==(const OrderAccepted &) const = default;
};

struct OrderCanceled {
    SeqNo seq;
    SymbolId symbol;
    OrderId order_id;
    bool operator==(const OrderCanceled &) const = default;
};

struct OrderModified {
    SeqNo seq;
    SymbolId symbol;
    OrderId order_id;
    bool operator==(const OrderModified &) const = default;
};

struct TradeEvent {
    SeqNo seq;
    SymbolId symbol;
    OrderId taker_id;
    OrderId maker_id;
    Price price;
    Quantity quantity;
    bool operator==(const TradeEvent &) const = default;
};

using EngineEvent = std::variant<OrderAccepted, OrderCanceled, OrderModified, TradeEvent>;

/// Sequence number of any engine event.
[[nodiscard]] inline SeqNo seq_of(const EngineEvent &event) noexcept {
    return std::visit([](const auto &e) noexcept { return e.seq; }, event);
}

/// Symbol of any engine event.
[[nodiscard]] inline SymbolId symbol_of(const EngineEvent &event) noexcept {
    return std::visit([](const auto &e) noexcept { return e.symbol; }, event);
}

} // namespace qsl::engine

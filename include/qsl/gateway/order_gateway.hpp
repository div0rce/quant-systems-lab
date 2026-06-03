#pragma once

#include "qsl/core/result.hpp"
#include "qsl/core/types.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/risk.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace qsl::gateway {

using core::OrderId;
using core::OrderType;
using core::Price;
using core::Quantity;
using core::RejectReason;
using core::Side;
using core::SymbolId;
using core::TimeInForce;
using engine::EngineEvent;
using engine::MatchingEngine;
using engine::RiskConfig;

/// Outcome of submitting a command through the gateway. A rejection carries a structured
/// reason and no events; an acceptance carries the engine's resulting event stream.
struct GatewayResult {
    bool accepted;
    RejectReason reason;
    std::vector<EngineEvent> events;

    [[nodiscard]] static GatewayResult reject(RejectReason r) { return {false, r, {}}; }
    [[nodiscard]] static GatewayResult accept(std::vector<EngineEvent> events) {
        return {true, RejectReason::None, std::move(events)};
    }
};

struct NewOrderPreview {
    bool accepted;
    std::size_t fill_count;
};

/// In-process order gateway: applies deterministic risk checks before forwarding accepted
/// commands to the engine. Stateless beyond the engine reference and the risk config;
/// "duplicate" and "unknown order" are defined by current engine state (resting orders).
class OrderGateway {
  public:
    OrderGateway(MatchingEngine &engine, RiskConfig config) : engine_(engine), config_(config) {}

    GatewayResult new_limit(SymbolId symbol, OrderId id, Side side, Price price, Quantity quantity,
                            TimeInForce tif);
    GatewayResult new_market(SymbolId symbol, OrderId id, Side side, Quantity quantity);
    GatewayResult cancel(SymbolId symbol, OrderId id);
    GatewayResult modify(SymbolId symbol, OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] NewOrderPreview preview_new_order(SymbolId symbol, OrderId id, Side side,
                                                    Price price, Quantity quantity,
                                                    OrderType type) const;

  private:
    MatchingEngine &engine_;
    RiskConfig config_;
};

} // namespace qsl::gateway

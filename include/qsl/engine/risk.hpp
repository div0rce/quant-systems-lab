#pragma once

#include "qsl/core/result.hpp"
#include "qsl/core/types.hpp"

namespace qsl::engine {

using core::Price;
using core::Quantity;
using core::QuantityTotal;
using core::RejectReason;
using core::Side;

/// Deterministic pre-trade risk limits.
struct RiskConfig {
    Quantity max_order_quantity;
    QuantityTotal max_notional;
};

// Value-level pre-trade checks. Return RejectReason::None if acceptable, otherwise the
// first failing reason. Identity checks (unknown symbol, duplicate/unknown order) live in
// the gateway, which knows engine state.

[[nodiscard]] RejectReason check_limit_values(const RiskConfig &config, Price price,
                                              Quantity quantity) noexcept;
[[nodiscard]] RejectReason check_limit(const RiskConfig &config, Side side, Price price,
                                       Quantity quantity) noexcept;
[[nodiscard]] RejectReason check_market(const RiskConfig &config, Side side,
                                        Quantity quantity) noexcept;

} // namespace qsl::engine

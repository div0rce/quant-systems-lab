#pragma once

#include "qsl/core/result.hpp"
#include "qsl/core/types.hpp"

namespace qsl::core {

// Deterministic domain validity predicates. No wall-clock, no floating point.

[[nodiscard]] constexpr bool is_valid_price(Price p) noexcept {
    return p > 0;
}

[[nodiscard]] constexpr bool is_valid_quantity(Quantity q) noexcept {
    return q > 0;
}

/// Validate a limit-order price/quantity pair, returning the first failing reason.
[[nodiscard]] constexpr Result validate_limit(Price price, Quantity qty) noexcept {
    if (!is_valid_price(price)) {
        return Result::reject(RejectReason::InvalidPrice);
    }
    if (!is_valid_quantity(qty)) {
        return Result::reject(RejectReason::InvalidQuantity);
    }
    return Result::success();
}

} // namespace qsl::core

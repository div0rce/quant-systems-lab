#include "qsl/engine/risk.hpp"

#include "qsl/core/invariants.hpp"

namespace qsl::engine {

RejectReason check_limit(const RiskConfig &config, Side side, Price price,
                         Quantity quantity) noexcept {
    if (!core::is_valid(side)) {
        return RejectReason::InvalidSide;
    }
    if (!core::is_valid_price(price)) {
        return RejectReason::InvalidPrice;
    }
    if (!core::is_valid_quantity(quantity)) {
        return RejectReason::InvalidQuantity;
    }
    if (quantity > config.max_order_quantity) {
        return RejectReason::MaxQuantityExceeded;
    }
    // Overflow-safe notional check (price > 0 here): notional = price * quantity exceeds
    // max_notional iff quantity > max_notional / price.
    if (static_cast<QuantityTotal>(quantity) >
        config.max_notional / static_cast<QuantityTotal>(price)) {
        return RejectReason::MaxNotionalExceeded;
    }
    return RejectReason::None;
}

RejectReason check_market(const RiskConfig &config, Side side, Quantity quantity) noexcept {
    if (!core::is_valid(side)) {
        return RejectReason::InvalidSide;
    }
    if (!core::is_valid_quantity(quantity)) {
        return RejectReason::InvalidQuantity;
    }
    if (quantity > config.max_order_quantity) {
        return RejectReason::MaxQuantityExceeded;
    }
    return RejectReason::None; // market orders have no price -> no notional check here
}

} // namespace qsl::engine

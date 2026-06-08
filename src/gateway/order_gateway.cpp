#include "qsl/gateway/order_gateway.hpp"

namespace qsl::gateway {

GatewayResult OrderGateway::new_limit(SymbolId symbol, OrderId id, Side side, Price price,
                                      Quantity quantity, TimeInForce tif) {
    if (!engine_.has_symbol(symbol)) {
        return GatewayResult::reject(RejectReason::UnknownSymbol);
    }
    if (engine_.contains(symbol, id)) {
        return GatewayResult::reject(RejectReason::DuplicateOrderId);
    }
    if (const RejectReason r = engine::check_limit(config_, side, price, quantity);
        r != RejectReason::None) {
        return GatewayResult::reject(r);
    }
    if (!engine_.can_store_limit(symbol, side, price, quantity, tif)) {
        return GatewayResult::reject(RejectReason::StorageExhausted);
    }
    return GatewayResult::accept(engine_.new_limit(symbol, id, side, price, quantity, tif));
}

GatewayResult OrderGateway::new_market(SymbolId symbol, OrderId id, Side side, Quantity quantity) {
    if (!engine_.has_symbol(symbol)) {
        return GatewayResult::reject(RejectReason::UnknownSymbol);
    }
    if (engine_.contains(symbol, id)) {
        return GatewayResult::reject(RejectReason::DuplicateOrderId);
    }
    if (const RejectReason r = engine::check_market(config_, side, quantity);
        r != RejectReason::None) {
        return GatewayResult::reject(r);
    }
    return GatewayResult::accept(engine_.new_market(symbol, id, side, quantity));
}

GatewayResult OrderGateway::cancel(SymbolId symbol, OrderId id) {
    if (!engine_.has_symbol(symbol)) {
        return GatewayResult::reject(RejectReason::UnknownSymbol);
    }
    if (!engine_.contains(symbol, id)) {
        return GatewayResult::reject(RejectReason::UnknownOrder);
    }
    return GatewayResult::accept(engine_.cancel(symbol, id));
}

GatewayResult OrderGateway::modify(SymbolId symbol, OrderId id, Price new_price,
                                   Quantity new_quantity) {
    if (!engine_.has_symbol(symbol)) {
        return GatewayResult::reject(RejectReason::UnknownSymbol);
    }
    if (!engine_.contains(symbol, id)) {
        return GatewayResult::reject(RejectReason::UnknownOrder);
    }
    if (new_quantity == 0) {
        return GatewayResult::accept(engine_.modify(symbol, id, new_price, new_quantity));
    }
    if (const RejectReason r = engine::check_limit_values(config_, new_price, new_quantity);
        r != RejectReason::None) {
        return GatewayResult::reject(r);
    }
    return GatewayResult::accept(engine_.modify(symbol, id, new_price, new_quantity));
}

NewOrderPreview OrderGateway::preview_new_order(SymbolId symbol, OrderId id, Side side, Price price,
                                                Quantity quantity, OrderType type,
                                                TimeInForce tif) const {
    if (!engine_.has_symbol(symbol) || engine_.contains(symbol, id)) {
        return NewOrderPreview{/*accepted=*/false, /*fill_count=*/0};
    }
    const RejectReason reason = (type == OrderType::Market)
                                    ? engine::check_market(config_, side, quantity)
                                    : engine::check_limit(config_, side, price, quantity);
    if (reason != RejectReason::None) {
        return NewOrderPreview{/*accepted=*/false, /*fill_count=*/0};
    }
    if (type == OrderType::Limit && !engine_.can_store_limit(symbol, side, price, quantity, tif)) {
        return NewOrderPreview{/*accepted=*/false, /*fill_count=*/0};
    }
    return NewOrderPreview{/*accepted=*/true,
                           /*fill_count=*/engine_.fill_count(symbol, side, price, type, quantity)};
}

} // namespace qsl::gateway

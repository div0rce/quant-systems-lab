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
    return GatewayResult::accept(engine_.modify(symbol, id, new_price, new_quantity));
}

} // namespace qsl::gateway

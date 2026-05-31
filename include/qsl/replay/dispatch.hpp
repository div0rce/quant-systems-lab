#pragma once

#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"

#include <variant>

namespace qsl::replay {

/// Apply one recorded command to the engine via the risk gateway. `RegisterSymbol` registers
/// the symbol and yields an empty acceptance; every other command returns the gateway's result
/// unchanged. This centralizes the command-to-gateway-method dispatch that the exporters, the
/// shrinker's trade counter, and the differential self-test would otherwise each duplicate.
inline gateway::GatewayResult apply_command(engine::MatchingEngine &engine,
                                            gateway::OrderGateway &gw, const Command &command) {
    if (const auto *rs = std::get_if<RegisterSymbol>(&command)) {
        engine.register_symbol(rs->name);
        return gateway::GatewayResult::accept({});
    }
    if (const auto *nl = std::get_if<NewLimit>(&command)) {
        return gw.new_limit(nl->symbol, nl->id, nl->side, nl->price, nl->quantity, nl->tif);
    }
    if (const auto *nm = std::get_if<NewMarket>(&command)) {
        return gw.new_market(nm->symbol, nm->id, nm->side, nm->quantity);
    }
    if (const auto *cn = std::get_if<Cancel>(&command)) {
        return gw.cancel(cn->symbol, cn->id);
    }
    const auto &md = std::get<Modify>(command);
    return gw.modify(md.symbol, md.id, md.price, md.quantity);
}

} // namespace qsl::replay

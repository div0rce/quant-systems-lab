#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <type_traits>
#include <variant>

using namespace qsl;

namespace {

// Emit one normalized line per engine event (in emission order).
void emit_events(std::ostream &os, const std::vector<engine::EngineEvent> &events) {
    for (const auto &event : events) {
        std::visit(
            [&os](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, engine::OrderAccepted>) {
                    os << "accept " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderCanceled>) {
                    os << "cancel " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderModified>) {
                    os << "modify " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::TradeEvent>) {
                    os << "trade " << e.seq << " " << e.symbol << " " << e.taker_id << " "
                       << e.maker_id << " " << e.price << " " << e.quantity << "\n";
                }
            },
            event);
    }
}

} // namespace

// qsl-export-fixture [seed] [orders]
//
// Drives a deterministic synthetic flow through the risk gateway and writes a normalized,
// textual event-log fixture to stdout for the independent OCaml replay verifier. A low
// max_order_quantity makes some new orders reject, so the fixture exercises the
// "rejected order never rests" invariant. Engine-neutral: no engine code is modified.
int main(int argc, char **argv) {
    const std::uint64_t seed = (argc >= 2) ? std::stoull(argv[1]) : 42;
    const std::size_t orders = (argc >= 3) ? std::stoull(argv[2]) : 200;
    const core::SymbolId symbols = 4;
    const core::Quantity max_qty = 8; // generate_flow uses qty 1..10 -> qty 9/10 reject

    engine::MatchingEngine engine;
    gateway::OrderGateway gateway{engine, gateway::RiskConfig{max_qty, /*max_notional=*/1'000'000}};
    const auto flow = replay::generate_flow(seed, symbols, orders);

    std::ostream &os = std::cout;
    os << "# qsl normalized replay fixture (engine event log + rejected new-order ids)\n";
    os << "v 1\n";
    os << "meta seed " << seed << " symbols " << symbols << " orders " << orders << " max_qty "
       << max_qty << "\n";

    std::size_t trades = 0;
    const auto record = [&](core::OrderId id, bool is_new_order, const gateway::GatewayResult &r) {
        if (is_new_order && !r.accepted) {
            os << "reject " << id << " " << core::to_string(r.reason) << "\n";
        }
        for (const auto &event : r.events) {
            if (std::holds_alternative<engine::TradeEvent>(event)) {
                ++trades;
            }
        }
        emit_events(os, r.events);
    };

    for (const auto &command : flow) {
        if (const auto *rs = std::get_if<replay::RegisterSymbol>(&command)) {
            engine.register_symbol(rs->name);
        } else if (const auto *nl = std::get_if<replay::NewLimit>(&command)) {
            record(
                nl->id, true,
                gateway.new_limit(nl->symbol, nl->id, nl->side, nl->price, nl->quantity, nl->tif));
        } else if (const auto *nm = std::get_if<replay::NewMarket>(&command)) {
            record(nm->id, true, gateway.new_market(nm->symbol, nm->id, nm->side, nm->quantity));
        } else if (const auto *cn = std::get_if<replay::Cancel>(&command)) {
            record(cn->id, false, gateway.cancel(cn->symbol, cn->id));
        } else {
            const auto &md = std::get<replay::Modify>(command);
            record(md.id, false, gateway.modify(md.symbol, md.id, md.price, md.quantity));
        }
    }

    os << "summary last_seq " << engine.snapshot().last_seq << " trades " << trades << "\n";
    return 0;
}

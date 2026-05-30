#include "qsl/replay/fixture.hpp"

#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/recovery.hpp"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace qsl::replay {
namespace {

char side_ch(core::Side s) {
    return s == core::Side::Buy ? 'B' : 'S';
}
const char *tif_str(core::TimeInForce t) {
    return t == core::TimeInForce::GTC ? "GTC" : "IOC";
}
std::string px(core::Price p) {
    return std::to_string(p);
}
std::string opt_px(const std::optional<core::Price> &p) {
    return p ? std::to_string(*p) : "-";
}

// Emit one `evt` line per engine event (in emission order), counting trades.
void emit_events(std::ostream &os, const std::vector<engine::EngineEvent> &events,
                 std::size_t &trades) {
    for (const auto &event : events) {
        std::visit(
            [&](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, engine::OrderAccepted>) {
                    os << "evt accept " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderCanceled>) {
                    os << "evt cancel " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderModified>) {
                    os << "evt modify " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::TradeEvent>) {
                    os << "evt trade " << e.seq << " " << e.symbol << " " << e.taker_id << " "
                       << e.maker_id << " " << e.price << " " << e.quantity << "\n";
                    ++trades;
                }
            },
            event);
    }
}

} // namespace

void write_stream_fixture(std::ostream &os, const FixtureParams &p) {
    engine::MatchingEngine engine;
    gateway::OrderGateway gw{engine, gateway::RiskConfig{p.max_qty, p.max_notional}};
    const auto flow = generate_flow(p.seed, p.symbols, p.orders);

    os << "# qsl differential-testing fixture: command stream + events + rejections + snapshot\n";
    os << "version 1\n";
    os << "meta seed " << p.seed << " symbols " << p.symbols << " orders " << p.orders
       << " max_qty " << p.max_qty << " max_notional " << p.max_notional << "\n";

    std::size_t trades = 0;
    for (const auto &command : flow) {
        if (const auto *rs = std::get_if<RegisterSymbol>(&command)) {
            os << "cmd reg " << rs->name << "\n";
            engine.register_symbol(rs->name);
        } else if (const auto *nl = std::get_if<NewLimit>(&command)) {
            os << "cmd limit " << nl->symbol << " " << nl->id << " " << side_ch(nl->side) << " "
               << px(nl->price) << " " << nl->quantity << " " << tif_str(nl->tif) << "\n";
            const auto r =
                gw.new_limit(nl->symbol, nl->id, nl->side, nl->price, nl->quantity, nl->tif);
            if (!r.accepted) {
                os << "reject " << nl->id << " " << core::to_string(r.reason) << "\n";
            }
            emit_events(os, r.events, trades);
        } else if (const auto *nm = std::get_if<NewMarket>(&command)) {
            os << "cmd market " << nm->symbol << " " << nm->id << " " << side_ch(nm->side) << " "
               << nm->quantity << "\n";
            const auto r = gw.new_market(nm->symbol, nm->id, nm->side, nm->quantity);
            if (!r.accepted) {
                os << "reject " << nm->id << " " << core::to_string(r.reason) << "\n";
            }
            emit_events(os, r.events, trades);
        } else if (const auto *cn = std::get_if<Cancel>(&command)) {
            os << "cmd cancel " << cn->symbol << " " << cn->id << "\n";
            emit_events(os, gw.cancel(cn->symbol, cn->id).events, trades);
        } else {
            const auto &md = std::get<Modify>(command);
            os << "cmd modify " << md.symbol << " " << md.id << " " << px(md.price) << " "
               << md.quantity << "\n";
            emit_events(os, gw.modify(md.symbol, md.id, md.price, md.quantity).events, trades);
        }
    }

    const auto snap = engine.snapshot();
    os << "snapshot last_seq " << snap.last_seq << " trades " << trades << "\n";
    for (const auto &s : snap.symbols) {
        os << "sym " << s.symbol << " bid " << opt_px(s.best_bid) << " ask " << opt_px(s.best_ask)
           << " orders " << s.order_count << "\n";
        for (const auto &lv : s.bids) {
            os << "level " << s.symbol << " B " << px(lv.price) << " " << lv.quantity << "\n";
        }
        for (const auto &lv : s.asks) {
            os << "level " << s.symbol << " A " << px(lv.price) << " " << lv.quantity << "\n";
        }
    }
}

} // namespace qsl::replay

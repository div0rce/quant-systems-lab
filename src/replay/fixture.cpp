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

void emit_reject(std::ostream &os, const char *kind, core::OrderId id, core::RejectReason reason) {
    os << "reject " << kind << " " << id << " " << core::to_string(reason) << "\n";
}

} // namespace

static void run_and_emit(std::ostream &os, const FixtureParams &p,
                         const std::vector<Command> &flow) {
    engine::MatchingEngine engine;
    gateway::OrderGateway gw{engine, gateway::RiskConfig{p.max_qty, p.max_notional}};

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
                emit_reject(os, "new_limit", nl->id, r.reason);
            } else {
                emit_events(os, r.events, trades);
            }
        } else if (const auto *nm = std::get_if<NewMarket>(&command)) {
            os << "cmd market " << nm->symbol << " " << nm->id << " " << side_ch(nm->side) << " "
               << nm->quantity << "\n";
            const auto r = gw.new_market(nm->symbol, nm->id, nm->side, nm->quantity);
            if (!r.accepted) {
                emit_reject(os, "new_market", nm->id, r.reason);
            } else {
                emit_events(os, r.events, trades);
            }
        } else if (const auto *cn = std::get_if<Cancel>(&command)) {
            os << "cmd cancel " << cn->symbol << " " << cn->id << "\n";
            const auto r = gw.cancel(cn->symbol, cn->id);
            if (!r.accepted) {
                emit_reject(os, "cancel", cn->id, r.reason);
            } else {
                emit_events(os, r.events, trades);
            }
        } else {
            const auto &md = std::get<Modify>(command);
            os << "cmd modify " << md.symbol << " " << md.id << " " << px(md.price) << " "
               << md.quantity << "\n";
            const auto r = gw.modify(md.symbol, md.id, md.price, md.quantity);
            if (!r.accepted) {
                emit_reject(os, "modify", md.id, r.reason);
            } else {
                emit_events(os, r.events, trades);
            }
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

void write_stream_fixture(std::ostream &os, const FixtureParams &p) {
    run_and_emit(os, p, generate_flow(p.seed, p.symbols, p.orders));
}

void write_ioc_scenario_fixture(std::ostream &os) {
    using core::Side;
    using core::TimeInForce;
    FixtureParams p;
    p.seed = 0;
    p.symbols = 1;
    p.orders = 6;
    p.max_qty = 1000;
    p.max_notional = 1'000'000;
    // Hand-built scenario exercising IOC discard (partial + no-cross), market, partial maker.
    const std::vector<Command> flow = {
        RegisterSymbol{"S0"},
        NewLimit{0, 1, Side::Sell, 100, 3, TimeInForce::GTC}, // rests ask 100x3
        NewLimit{0, 2, Side::Buy, 100, 5, TimeInForce::IOC},  // fills 3, discards 2
        NewLimit{0, 3, Side::Buy, 99, 4, TimeInForce::IOC},   // no cross, discarded
        NewLimit{0, 4, Side::Sell, 101, 5, TimeInForce::GTC}, // rests ask 101x5
        NewLimit{0, 5, Side::Buy, 101, 2, TimeInForce::IOC},  // fills 2, ask 101 -> 3
        NewMarket{0, 6, Side::Buy, 1},                        // fills 1, ask 101 -> 2
    };
    run_and_emit(os, p, flow);
}

void write_property_fixture(std::ostream &os, std::uint64_t seed) {
    FixtureParams p;
    p.seed = seed;
    p.symbols = 3;
    p.orders = 120;
    p.max_qty = 20;        // qty 50 -> MaxQuantityExceeded
    p.max_notional = 1000; // large valid orders -> MaxNotionalExceeded
    run_and_emit(os, p, generate_property_flow(seed, p.symbols, p.orders));
}

} // namespace qsl::replay

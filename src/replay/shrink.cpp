#include "qsl/replay/shrink.hpp"

#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/recovery.hpp"

#include <utility>
#include <variant>

namespace qsl::replay {
namespace {

// Remove the half-open range [i, i+n) from `v`.
std::vector<Command> without(const std::vector<Command> &v, std::size_t i, std::size_t n) {
    std::vector<Command> out;
    out.reserve(v.size() - n);
    out.insert(out.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i));
    out.insert(out.end(), v.begin() + static_cast<std::ptrdiff_t>(i + n), v.end());
    return out;
}

// Try lowering a command's quantity to 1 (a common minimizing simplification).
Command with_min_qty(const Command &c) {
    if (const auto *nl = std::get_if<NewLimit>(&c)) {
        NewLimit n = *nl;
        n.quantity = 1;
        return n;
    }
    if (const auto *nm = std::get_if<NewMarket>(&c)) {
        NewMarket n = *nm;
        n.quantity = 1;
        return n;
    }
    if (const auto *md = std::get_if<Modify>(&c)) {
        Modify n = *md;
        n.quantity = 1;
        return n;
    }
    return c;
}

} // namespace

std::vector<Command> shrink(std::vector<Command> cur, const ShrinkPredicate &fails) {
    if (!fails(cur)) {
        return cur; // predicate does not hold; nothing to shrink
    }
    bool changed = true;
    while (changed) {
        changed = false;
        // (1)+(2) remove contiguous chunks of decreasing size (n==1 is single-command removal).
        for (std::size_t n = (cur.size() > 1 ? cur.size() / 2 : 1); n >= 1; n /= 2) {
            std::size_t i = 0;
            while (i + n <= cur.size()) {
                auto candidate = without(cur, i, n);
                if (fails(candidate)) {
                    cur = std::move(candidate);
                    changed = true; // keep i: the next chunk now begins at i
                } else {
                    i += n;
                }
            }
            if (n == 1) {
                break;
            }
        }
        // (3) simplify fields: lower quantities where the predicate still holds.
        for (std::size_t i = 0; i < cur.size(); ++i) {
            auto candidate = cur;
            candidate[i] = with_min_qty(cur[i]);
            if (candidate[i] != cur[i] && fails(candidate)) {
                cur = std::move(candidate);
                changed = true;
            }
        }
    }
    return cur;
}

std::size_t count_trades(const std::vector<Command> &commands, core::Quantity max_qty,
                         core::QuantityTotal max_notional) {
    engine::MatchingEngine engine;
    gateway::OrderGateway gw{engine, gateway::RiskConfig{max_qty, max_notional}};
    std::size_t trades = 0;
    for (const auto &command : commands) {
        if (const auto *rs = std::get_if<RegisterSymbol>(&command)) {
            engine.register_symbol(rs->name);
            continue;
        }
        gateway::GatewayResult r{true, core::RejectReason::None, {}};
        if (const auto *nl = std::get_if<NewLimit>(&command)) {
            r = gw.new_limit(nl->symbol, nl->id, nl->side, nl->price, nl->quantity, nl->tif);
        } else if (const auto *nm = std::get_if<NewMarket>(&command)) {
            r = gw.new_market(nm->symbol, nm->id, nm->side, nm->quantity);
        } else if (const auto *cn = std::get_if<Cancel>(&command)) {
            r = gw.cancel(cn->symbol, cn->id);
        } else {
            const auto &md = std::get<Modify>(command);
            r = gw.modify(md.symbol, md.id, md.price, md.quantity);
        }
        for (const auto &event : r.events) {
            if (std::holds_alternative<engine::TradeEvent>(event)) {
                ++trades;
            }
        }
    }
    return trades;
}

} // namespace qsl::replay

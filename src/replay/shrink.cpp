#include "qsl/replay/shrink.hpp"

#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/dispatch.hpp"
#include "qsl/replay/recovery.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

// Try lowering a limit/modify command's price to 1 (a minimizing simplification; market orders
// carry no price). Always predicate-guarded by the caller, so a price change that breaks the
// failure is discarded.
Command with_min_price(const Command &c) {
    if (const auto *nl = std::get_if<NewLimit>(&c)) {
        NewLimit n = *nl;
        n.price = 1;
        return n;
    }
    if (const auto *md = std::get_if<Modify>(&c)) {
        Modify n = *md;
        n.price = 1;
        return n;
    }
    return c;
}

} // namespace

std::vector<Command> renumber(const std::vector<Command> &cmds) {
    // The positional id model below (the i-th RegisterSymbol owns symbol id i) only holds when
    // every symbol name is distinct. Registration is idempotent, so a repeated name does NOT
    // allocate a new id; renumbering such a stream could turn an UnknownSymbol reject into an
    // accepted order and misrepresent the original replay. Bail in that case (the removal passes
    // still drop idempotent duplicate registrations).
    {
        std::set<std::string> names;
        for (const auto &c : cmds) {
            if (const auto *rs = std::get_if<RegisterSymbol>(&c)) {
                if (!names.insert(rs->name).second) {
                    return cmds;
                }
            }
        }
    }
    // Symbols referenced by non-register commands (the i-th RegisterSymbol owns symbol id i).
    std::set<core::SymbolId> referenced;
    for (const auto &c : cmds) {
        if (const auto *nl = std::get_if<NewLimit>(&c)) {
            referenced.insert(nl->symbol);
        } else if (const auto *nm = std::get_if<NewMarket>(&c)) {
            referenced.insert(nm->symbol);
        } else if (const auto *cn = std::get_if<Cancel>(&c)) {
            referenced.insert(cn->symbol);
        } else if (const auto *md = std::get_if<Modify>(&c)) {
            referenced.insert(md->symbol);
        }
    }
    std::map<core::SymbolId, core::SymbolId> sym_remap;
    core::SymbolId old_sym = 0;
    core::SymbolId new_sym = 0;
    for (const auto &c : cmds) {
        if (std::holds_alternative<RegisterSymbol>(c)) {
            if (referenced.count(old_sym) != 0) {
                sym_remap[old_sym] = new_sym++;
            }
            ++old_sym;
        }
    }
    const auto map_sym = [&](core::SymbolId s) {
        const auto it = sym_remap.find(s);
        return it == sym_remap.end() ? s : it->second; // unregistered refs keep their id
    };

    // Order ids compacted in first-appearance order (a bijection preserves duplicate/cancel/modify
    // matching).
    std::map<core::OrderId, core::OrderId> id_remap;
    core::OrderId next_id = 0;
    const auto map_id = [&](core::OrderId id) {
        const auto [it, inserted] = id_remap.try_emplace(id, next_id);
        if (inserted) {
            ++next_id;
        }
        return it->second;
    };

    std::vector<Command> out;
    out.reserve(cmds.size());
    old_sym = 0;
    for (const auto &c : cmds) {
        if (std::holds_alternative<RegisterSymbol>(c)) {
            const bool keep = referenced.count(old_sym) != 0;
            const core::SymbolId ns = keep ? sym_remap[old_sym] : 0;
            ++old_sym;
            if (keep) {
                out.push_back(RegisterSymbol{"S" + std::to_string(ns)});
            }
        } else if (const auto *nl = std::get_if<NewLimit>(&c)) {
            out.push_back(NewLimit{map_sym(nl->symbol), map_id(nl->id), nl->side, nl->price,
                                   nl->quantity, nl->tif});
        } else if (const auto *nm = std::get_if<NewMarket>(&c)) {
            out.push_back(NewMarket{map_sym(nm->symbol), map_id(nm->id), nm->side, nm->quantity});
        } else if (const auto *cn = std::get_if<Cancel>(&c)) {
            out.push_back(Cancel{map_sym(cn->symbol), map_id(cn->id)});
        } else {
            const auto &md = std::get<Modify>(c);
            out.push_back(Modify{map_sym(md.symbol), map_id(md.id), md.price, md.quantity});
        }
    }
    return out;
}

std::vector<Command> shrink(std::vector<Command> cur, const ShrinkPredicate &fails,
                            std::size_t *iterations) {
    if (!fails(cur)) {
        if (iterations != nullptr) {
            *iterations = 0;
        }
        return cur; // predicate does not hold; nothing to shrink
    }
    std::size_t passes = 0;
    bool changed = true;
    while (changed) {
        ++passes;
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
        // (4) simplify fields: lower limit/modify prices where the predicate still holds.
        for (std::size_t i = 0; i < cur.size(); ++i) {
            auto candidate = cur;
            candidate[i] = with_min_price(cur[i]);
            if (candidate[i] != cur[i] && fails(candidate)) {
                cur = std::move(candidate);
                changed = true;
            }
        }
        // (5) renumber: drop unreferenced symbol registrations and compact symbol/order ids.
        {
            auto candidate = renumber(cur);
            if (candidate != cur && fails(candidate)) {
                cur = std::move(candidate);
                changed = true;
            }
        }
    }
    if (iterations != nullptr) {
        *iterations = passes;
    }
    return cur;
}

std::size_t count_trades(const std::vector<Command> &commands, core::Quantity max_qty,
                         core::QuantityTotal max_notional) {
    engine::MatchingEngine engine;
    gateway::OrderGateway gw{engine, gateway::RiskConfig{max_qty, max_notional}};
    std::size_t trades = 0;
    for (const auto &command : commands) {
        for (const auto &event : apply_command(engine, gw, command).events) {
            if (std::holds_alternative<engine::TradeEvent>(event)) {
                ++trades;
            }
        }
    }
    return trades;
}

} // namespace qsl::replay

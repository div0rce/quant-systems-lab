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

bool has_duplicate_symbol_registration_names(const std::vector<Command> &cmds) {
    std::set<std::string> names;
    for (const auto &c : cmds) {
        if (const auto *rs = std::get_if<RegisterSymbol>(&c)) {
            if (!names.insert(rs->name).second) {
                return true;
            }
        }
    }
    return false;
}

std::set<core::SymbolId> collect_referenced_symbols(const std::vector<Command> &cmds) {
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
    return referenced;
}

std::map<core::SymbolId, core::SymbolId>
build_symbol_remap(const std::vector<Command> &cmds, const std::set<core::SymbolId> &referenced) {
    std::map<core::SymbolId, core::SymbolId> remap;
    core::SymbolId old_sym = 0;
    core::SymbolId new_sym = 0;
    for (const auto &c : cmds) {
        if (std::holds_alternative<RegisterSymbol>(c)) {
            if (referenced.count(old_sym) != 0) {
                remap[old_sym] = new_sym++;
            }
            ++old_sym;
        }
    }
    return remap;
}

core::SymbolId remap_symbol(const std::map<core::SymbolId, core::SymbolId> &remap,
                            core::SymbolId symbol) {
    const auto it = remap.find(symbol);
    return it == remap.end() ? symbol : it->second; // unregistered refs keep their id
}

class OrderIdRemapper {
  public:
    core::OrderId map(core::OrderId id) {
        const auto [it, inserted] = remap_.try_emplace(id, next_id_);
        if (inserted) {
            ++next_id_;
        }
        return it->second;
    }

  private:
    std::map<core::OrderId, core::OrderId> remap_;
    core::OrderId next_id_ = 0;
};

Command remap_non_register_command(const Command &c,
                                   const std::map<core::SymbolId, core::SymbolId> &sym_remap,
                                   OrderIdRemapper &id_remap) {
    if (const auto *nl = std::get_if<NewLimit>(&c)) {
        return NewLimit{remap_symbol(sym_remap, nl->symbol),
                        id_remap.map(nl->id),
                        nl->side,
                        nl->price,
                        nl->quantity,
                        nl->tif};
    }
    if (const auto *nm = std::get_if<NewMarket>(&c)) {
        return NewMarket{remap_symbol(sym_remap, nm->symbol), id_remap.map(nm->id), nm->side,
                         nm->quantity};
    }
    if (const auto *cn = std::get_if<Cancel>(&c)) {
        return Cancel{remap_symbol(sym_remap, cn->symbol), id_remap.map(cn->id)};
    }
    const auto &md = std::get<Modify>(c);
    return Modify{remap_symbol(sym_remap, md.symbol), id_remap.map(md.id), md.price, md.quantity};
}

void append_renumbered_registration(std::vector<Command> &out, core::SymbolId old_sym,
                                    const std::map<core::SymbolId, core::SymbolId> &sym_remap) {
    const auto it = sym_remap.find(old_sym);
    if (it != sym_remap.end()) {
        out.push_back(RegisterSymbol{"S" + std::to_string(it->second)});
    }
}

bool try_remove_chunks_of_size(std::vector<Command> &cur, const ShrinkPredicate &fails,
                               std::size_t chunk_size) {
    bool changed = false;
    std::size_t i = 0;
    while (i + chunk_size <= cur.size()) {
        auto candidate = without(cur, i, chunk_size);
        if (fails(candidate)) {
            cur = std::move(candidate);
            changed = true; // keep i: the next chunk now begins at i
        } else {
            i += chunk_size;
        }
    }
    return changed;
}

bool remove_multi_command_chunks(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    bool changed = false;
    for (std::size_t n = cur.size() > 1 ? cur.size() / 2 : 1; n > 1; n /= 2) {
        changed = try_remove_chunks_of_size(cur, fails, n) || changed;
    }
    return changed;
}

bool remove_single_commands(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    return try_remove_chunks_of_size(cur, fails, 1);
}

bool remove_contiguous_ranges(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    bool changed = remove_multi_command_chunks(cur, fails);
    changed = remove_single_commands(cur, fails) || changed;
    return changed;
}

using CommandSimplifier = Command (*)(const Command &);

bool simplify_fields(std::vector<Command> &cur, const ShrinkPredicate &fails,
                     CommandSimplifier simplify) {
    bool changed = false;
    for (std::size_t i = 0; i < cur.size(); ++i) {
        auto candidate = cur;
        candidate[i] = simplify(cur[i]);
        if (candidate[i] != cur[i] && fails(candidate)) {
            cur = std::move(candidate);
            changed = true;
        }
    }
    return changed;
}

bool simplify_quantities(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    return simplify_fields(cur, fails, with_min_qty);
}

bool simplify_prices(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    return simplify_fields(cur, fails, with_min_price);
}

bool renumber_symbols_and_orders(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    auto candidate = renumber(cur);
    if (candidate != cur && fails(candidate)) {
        cur = std::move(candidate);
        return true;
    }
    return false;
}

bool run_reduction_pass(std::vector<Command> &cur, const ShrinkPredicate &fails) {
    bool changed = remove_contiguous_ranges(cur, fails);
    changed = simplify_quantities(cur, fails) || changed;
    changed = simplify_prices(cur, fails) || changed;
    changed = renumber_symbols_and_orders(cur, fails) || changed;
    return changed;
}

} // namespace

std::vector<Command> renumber(const std::vector<Command> &cmds) {
    // The positional id model below (the i-th RegisterSymbol owns symbol id i) only holds when
    // every symbol name is distinct. Registration is idempotent, so a repeated name does NOT
    // allocate a new id; renumbering such a stream could turn an UnknownSymbol reject into an
    // accepted order and misrepresent the original replay. Bail in that case (the removal passes
    // still drop idempotent duplicate registrations).
    if (has_duplicate_symbol_registration_names(cmds)) {
        return cmds;
    }
    // Symbols referenced by non-register commands (the i-th RegisterSymbol owns symbol id i).
    const auto referenced = collect_referenced_symbols(cmds);
    const auto sym_remap = build_symbol_remap(cmds, referenced);
    OrderIdRemapper id_remap;

    std::vector<Command> out;
    out.reserve(cmds.size());
    core::SymbolId old_sym = 0;
    for (const auto &c : cmds) {
        if (std::holds_alternative<RegisterSymbol>(c)) {
            append_renumbered_registration(out, old_sym, sym_remap);
            ++old_sym;
        } else {
            out.push_back(remap_non_register_command(c, sym_remap, id_remap));
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
        changed = run_reduction_pass(cur, fails);
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

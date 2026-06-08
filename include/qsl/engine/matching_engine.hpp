#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/order_book.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qsl::engine {

using core::OrderType;

/// Interns external symbol names to compact numeric SymbolIds (assigned in
/// registration order).
class SymbolRegistry {
  public:
    SymbolId intern(std::string_view name);
    [[nodiscard]] std::optional<SymbolId> find(std::string_view name) const;

  private:
    std::unordered_map<std::string, SymbolId> ids_;
    SymbolId next_{0};
};

/// Per-symbol view of the book used for deterministic snapshot comparison.
struct SymbolSnapshot {
    SymbolId symbol;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::size_t order_count;
    std::vector<LevelView> bids; // best (highest) first
    std::vector<LevelView> asks; // best (lowest) first
    bool operator==(const SymbolSnapshot &) const = default;
};

/// Deterministic engine state snapshot (symbols ordered by SymbolId). Extended
/// with per-level aggregates and trade-sequence comparison in M8.
struct EngineSnapshot {
    SeqNo last_seq;
    std::vector<SymbolSnapshot> symbols;
    bool operator==(const EngineSnapshot &) const = default;
};

/// Multi-symbol matching engine. Routes commands to per-symbol books and emits a
/// strictly-increasing, deterministic engine event stream. No wall-clock dependence.
class MatchingEngine {
  public:
    // Storage::Pooled routes every per-symbol book's node storage through a pooled memory resource
    // (the M32 experiment); Baseline keeps operator new/delete. Default Baseline leaves all
    // existing behavior unchanged. Matching semantics and determinism are identical either way.
    explicit MatchingEngine(OrderBook::Storage storage = OrderBook::Storage::Baseline);

    SymbolId register_symbol(std::string_view name);
    [[nodiscard]] std::optional<SymbolId> symbol_id(std::string_view name) const;

    std::vector<EngineEvent> new_limit(SymbolId symbol, OrderId id, Side side, Price price,
                                       Quantity quantity, TimeInForce tif);
    std::vector<EngineEvent> new_market(SymbolId symbol, OrderId id, Side side, Quantity quantity);
    std::vector<EngineEvent> cancel(SymbolId symbol, OrderId id);
    std::vector<EngineEvent> modify(SymbolId symbol, OrderId id, Price new_price,
                                    Quantity new_quantity);

    [[nodiscard]] SeqNo last_seq() const noexcept { return seq_; }
    [[nodiscard]] EngineSnapshot snapshot() const;

    [[nodiscard]] bool has_symbol(SymbolId symbol) const;
    [[nodiscard]] bool contains(SymbolId symbol, OrderId id) const;

    [[nodiscard]] std::optional<Price> best_bid(SymbolId symbol) const;
    [[nodiscard]] std::optional<Price> best_ask(SymbolId symbol) const;
    [[nodiscard]] std::size_t fill_count(SymbolId symbol, Side side, Price price, OrderType type,
                                         Quantity quantity) const;
    [[nodiscard]] bool can_store_limit(SymbolId symbol, Side side, Price price, Quantity quantity,
                                       TimeInForce tif) const;

  private:
    OrderBook *find_book(SymbolId symbol) noexcept;
    SeqNo next_seq() noexcept { return ++seq_; }

    SymbolRegistry registry_;
    std::map<SymbolId, OrderBook> books_; // ordered -> deterministic snapshot
    SeqNo seq_{0};
    OrderBook::Storage book_storage_{OrderBook::Storage::Baseline};
};

} // namespace qsl::engine

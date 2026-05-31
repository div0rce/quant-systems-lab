#pragma once

#include "qsl/core/types.hpp"
#include "qsl/replay/command.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace qsl::replay {

// A failure predicate over a command stream: true when the stream still "fails" (reproduces
// the property of interest). In real differential use this would be "C++ and OCaml snapshots
// disagree"; the tests use an artificial predicate since the engines currently agree.
using ShrinkPredicate = std::function<bool(const std::vector<Command> &)>;

// Greedy, deterministic shrink: returns a reduced command stream for which `fails` still holds.
// Strategies (iterated to a fixed point): remove contiguous chunks, remove single commands,
// and simplify command fields (lower quantities). Not guaranteed globally minimal. If
// `iterations` is non-null, it receives the number of fixed-point passes performed.
[[nodiscard]] std::vector<Command> shrink(std::vector<Command> commands,
                                          const ShrinkPredicate &fails,
                                          std::size_t *iterations = nullptr);

// Replay `commands` through the risk gateway and return the number of trades produced.
// A convenient building block for predicates.
[[nodiscard]] std::size_t count_trades(const std::vector<Command> &commands, core::Quantity max_qty,
                                       core::QuantityTotal max_notional);

// Canonicalize symbol and order ids to shrink a fixture further: drop RegisterSymbol commands for
// symbols no other command references, renumber the survivors to 0..k-1 (renamed S0..), and
// compact order ids to first-appearance order (both bijective, so engine semantics are preserved).
// Returns the input unchanged if any symbol name is registered more than once, since idempotent
// registration would break the positional id model. Exposed for testing; `shrink` applies it
// predicate-guarded.
[[nodiscard]] std::vector<Command> renumber(const std::vector<Command> &commands);

} // namespace qsl::replay

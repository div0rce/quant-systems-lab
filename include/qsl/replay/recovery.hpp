#pragma once

#include "qsl/core/types.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qsl::replay {

using core::SymbolId;
using engine::EngineEvent;
using engine::MatchingEngine;

/// Version of the deterministic command generators below. Bump it whenever a change alters the
/// generated command streams, then regenerate the fixtures and the reproducibility manifest, so
/// fixture provenance (seed + version + hash) stays honest.
inline constexpr int kGeneratorVersion = 1;

/// Apply one recorded command to the engine, returning the emitted events.
[[nodiscard]] std::vector<EngineEvent> apply(MatchingEngine &engine, const Command &command);

/// Rebuild engine state by applying every Command record in order. Non-command records
/// and undecodable payloads are skipped. Returns the full replayed event stream.
[[nodiscard]] std::vector<EngineEvent> replay(MatchingEngine &engine,
                                              const std::vector<LogRecord> &records);

/// Deterministic synthetic order flow (fixed RNG seed) over `symbols` symbols. Begins with
/// the RegisterSymbol commands so the flow is self-contained and replayable.
[[nodiscard]] std::vector<Command> generate_flow(std::uint64_t seed, SymbolId symbols,
                                                 std::size_t orders);

/// Enriched deterministic flow for property/differential testing: covers IOC, invalid prices
/// and quantities, duplicate active ids, reused inactive ids, unknown symbols, cancels/modifies
/// of active and inactive orders, market orders, and multi-symbol interleavings. Both the C++
/// engine and the independent OCaml replay must agree on the resulting snapshot.
[[nodiscard]] std::vector<Command> generate_property_flow(std::uint64_t seed, SymbolId symbols,
                                                          std::size_t orders);

} // namespace qsl::replay

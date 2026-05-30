#pragma once

#include "qsl/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <ostream>

namespace qsl::replay {

// Parameters for a deterministic synthetic flow exported as a differential-testing fixture.
struct FixtureParams {
    std::uint64_t seed = 42;
    core::SymbolId symbols = 4;
    std::size_t orders = 200;
    core::Quantity max_qty = 8; // generate_flow uses qty 1..10 -> some new orders reject
    core::QuantityTotal max_notional = 1'000'000;
};

// Write a normalized differential-testing fixture for the flow defined by `params`:
// the symbol-registration + command stream, the emitted engine events, new-order
// rejections, and the final per-symbol snapshot (best bid/ask, per-price level aggregates,
// resting order counts, last sequence, trade count). The schema is documented in
// docs/differential_testing.md. Output is deterministic for a given `params`.
void write_stream_fixture(std::ostream &os, const FixtureParams &params);

// Write a small hand-built fixture exercising IOC (partial + no-cross), market, and partial
// maker fills, so the differential replay tests cover IOC (the synthetic flow uses only GTC).
void write_ioc_scenario_fixture(std::ostream &os);

// Write an enriched property-test fixture for the given seed (generate_property_flow), used to
// drive the cross-language differential tests over many seeds.
void write_property_fixture(std::ostream &os, std::uint64_t seed);

} // namespace qsl::replay

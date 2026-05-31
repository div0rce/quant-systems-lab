// Reject-reason coverage (issue #44). Tallies the rejection reasons the property generator
// produces across the committed seeds and asserts every reachable reason occurs at least once,
// so a generator change that silently stops exercising a rejection path fails CI. The frequency
// report is surfaced via Catch2 INFO (shown on failure / with `ctest -V`).

#include "qsl/core/result.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/dispatch.hpp"
#include "qsl/replay/recovery.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>

using namespace qsl;

TEST_CASE("the property generator exercises every reachable reject reason",
          "[selftest][coverage]") {
    std::map<core::RejectReason, int> freq;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        engine::MatchingEngine eng;
        gateway::OrderGateway gw{eng, gateway::RiskConfig{/*max_qty=*/20, /*max_notional=*/1000}};
        for (const auto &c : replay::generate_property_flow(seed, /*symbols=*/3, /*orders=*/120)) {
            const auto r = replay::apply_command(eng, gw, c);
            if (!r.accepted) {
                ++freq[r.reason];
            }
        }
    }

    // Frequency report (visible on failure / with ctest -V).
    for (const auto &[reason, n] : freq) {
        UNSCOPED_INFO(core::to_string(reason) << " = " << n);
    }

    // Every reason reachable from a well-typed command stream must occur. `None` is not a
    // rejection; `InvalidSide` requires a malformed wire-level side and is covered by the
    // protocol fuzz tests, not this typed replay generator.
    constexpr std::array reachable = {
        core::RejectReason::UnknownSymbol,       core::RejectReason::InvalidPrice,
        core::RejectReason::InvalidQuantity,     core::RejectReason::MaxQuantityExceeded,
        core::RejectReason::MaxNotionalExceeded, core::RejectReason::DuplicateOrderId,
        core::RejectReason::UnknownOrder,
    };
    for (const auto reason : reachable) {
        INFO("missing reject reason: " << core::to_string(reason));
        REQUIRE(freq[reason] > 0);
    }
}

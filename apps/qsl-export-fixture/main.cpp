#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/dispatch.hpp"
#include "qsl/replay/recovery.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace qsl;

namespace {

// Parse a whole token as an unsigned integer; rejects junk/trailing chars/overflow rather than
// std::stoull's throw-on-bad-input (which would call std::terminate from main).
std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t value = 0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

struct FixtureArgs {
    std::uint64_t seed;
    std::size_t orders;
};

// Optional [seed] [orders] args (defaults 42 / 200); nullopt if either is present but malformed.
std::optional<FixtureArgs> parse_args(int argc, char **argv) {
    const auto seed = (argc >= 2) ? parse_u64(argv[1]) : std::optional<std::uint64_t>{42};
    const auto orders = (argc >= 3) ? parse_u64(argv[2]) : std::optional<std::uint64_t>{200};
    if (!seed || !orders) {
        return std::nullopt;
    }
    return FixtureArgs{*seed, static_cast<std::size_t>(*orders)};
}

// Emit one normalized line per engine event (in emission order).
void emit_events(std::ostream &os, const std::vector<engine::EngineEvent> &events) {
    for (const auto &event : events) {
        std::visit(
            [&os](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, engine::OrderAccepted>) {
                    os << "accept " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderCanceled>) {
                    os << "cancel " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::OrderModified>) {
                    os << "modify " << e.seq << " " << e.symbol << " " << e.order_id << "\n";
                } else if constexpr (std::is_same_v<T, engine::TradeEvent>) {
                    os << "trade " << e.seq << " " << e.symbol << " " << e.taker_id << " "
                       << e.maker_id << " " << e.price << " " << e.quantity << "\n";
                }
            },
            event);
    }
}

} // namespace

// qsl-export-fixture [seed] [orders]
//
// Drives a deterministic synthetic flow through the risk gateway and writes a normalized,
// textual event-log fixture to stdout for the independent OCaml replay verifier. A low
// max_order_quantity makes some new orders reject, so the fixture exercises the
// "rejected order never rests" invariant. Engine-neutral: no engine code is modified.
int main(int argc, char **argv) {
    const auto args = parse_args(argc, argv);
    if (!args) {
        std::cerr << "usage: qsl-export-fixture [seed] [orders]\n";
        return 2;
    }
    const std::uint64_t seed = args->seed;
    const std::size_t orders = args->orders;
    const core::SymbolId symbols = 4;
    const core::Quantity max_qty = 8; // generate_flow can emit qty > 8 -> some orders reject

    engine::MatchingEngine engine;
    gateway::OrderGateway gateway{engine, gateway::RiskConfig{max_qty, /*max_notional=*/1'000'000}};
    const auto flow = replay::generate_flow(seed, symbols, orders);

    std::ostream &os = std::cout;
    os << "# qsl normalized replay fixture (engine event log + rejected new-order ids)\n";
    os << "v 1\n";
    os << "meta seed " << seed << " symbols " << symbols << " orders " << orders << " max_qty "
       << max_qty << "\n";

    std::size_t trades = 0;
    const auto record = [&](core::OrderId id, bool is_new_order, const gateway::GatewayResult &r) {
        if (is_new_order && !r.accepted) {
            os << "reject " << id << " " << core::to_string(r.reason) << "\n";
        }
        for (const auto &event : r.events) {
            if (std::holds_alternative<engine::TradeEvent>(event)) {
                ++trades;
            }
        }
        emit_events(os, r.events);
    };

    for (const auto &command : flow) {
        const auto r = replay::apply_command(engine, gateway, command);
        if (const auto *nl = std::get_if<replay::NewLimit>(&command)) {
            record(nl->id, true, r);
        } else if (const auto *nm = std::get_if<replay::NewMarket>(&command)) {
            record(nm->id, true, r);
        } else if (const auto *cn = std::get_if<replay::Cancel>(&command)) {
            record(cn->id, false, r);
        } else if (const auto *md = std::get_if<replay::Modify>(&command)) {
            record(md->id, false, r);
        }
        // RegisterSymbol: apply_command already registered it; it produces no log line.
    }

    os << "summary last_seq " << engine.snapshot().last_seq << " trades " << trades << "\n";
    return 0;
}

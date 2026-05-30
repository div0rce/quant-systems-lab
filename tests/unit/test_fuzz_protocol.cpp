#include "qsl/engine/matching_engine.hpp"
#include "qsl/feed/market_data.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/session.hpp"
#include "qsl/protocol/codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {
std::vector<std::byte> random_bytes(std::mt19937_64 &rng, std::size_t len) {
    std::vector<std::byte> buf(len);
    for (auto &b : buf) {
        b = static_cast<std::byte>(static_cast<std::uint8_t>(rng()));
    }
    return buf;
}
} // namespace

// The decoders are non-throwing and bounds-safe; on random input they must return a
// deterministic error without reading out of bounds. Run under `make asan` (ASan/UBSan) this
// also proves no undefined behavior. Reaching the end without crashing is the assertion.
TEST_CASE("decoders never crash on random bytes", "[fuzz]") {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        const auto buf = random_bytes(rng, rng() % 64);
        const std::span<const std::byte> in{buf};
        static_cast<void>(qsl::protocol::decode_header(in));
        static_cast<void>(qsl::protocol::decode_new_order(in));
        static_cast<void>(qsl::protocol::decode_cancel_order(in));
        static_cast<void>(qsl::protocol::decode_heartbeat(in));
        static_cast<void>(qsl::protocol::decode_ack(in));
        static_cast<void>(qsl::protocol::decode_reject(in));
        static_cast<void>(qsl::protocol::decode_fill(in));
        static_cast<void>(qsl::feed::decode_market_data(in));
    }
    SUCCEED("no crash on random decoder input");
}

TEST_CASE("gateway session never crashes on random bytes", "[fuzz]") {
    std::mt19937_64 rng(999);
    for (int i = 0; i < 5000; ++i) {
        qsl::engine::MatchingEngine eng;
        eng.register_symbol("AAPL");
        qsl::gateway::OrderGateway gw{eng, qsl::gateway::RiskConfig{1000, 1'000'000}};
        qsl::gateway::Session session{gw};
        const auto buf = random_bytes(rng, rng() % 80);
        static_cast<void>(session.on_bytes(buf));
    }
    SUCCEED("no crash on random session input");
}

TEST_CASE("a session survives random bytes fed in random chunks", "[fuzz]") {
    std::mt19937_64 rng(0xC0FFEE);
    qsl::engine::MatchingEngine eng;
    eng.register_symbol("AAPL");
    qsl::gateway::OrderGateway gw{eng, qsl::gateway::RiskConfig{1000, 1'000'000}};
    qsl::gateway::Session session{gw};
    for (int i = 0; i < 5000 && !session.logged_out(); ++i) {
        const auto buf = random_bytes(rng, 1 + (rng() % 7)); // small chunks exercise buffering
        static_cast<void>(session.on_bytes(buf));
    }
    SUCCEED("no crash feeding random chunks to one session");
}

#include "qsl/engine/matching_engine.hpp"
#include "qsl/feed/market_data.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/session.hpp"
#include "qsl/protocol/codec.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace qsl;
using protocol::DecodeError;

namespace {

std::vector<std::byte> random_bytes(std::mt19937_64 &rng, std::size_t len) {
    std::vector<std::byte> buf(len);
    for (auto &b : buf) {
        b = static_cast<std::byte>(static_cast<std::uint8_t>(rng()));
    }
    return buf;
}

// A valid, gateway-acceptable NewOrder frame (symbol 0, positive price, small quantity).
std::vector<std::byte> valid_new_order_frame(core::OrderId id) {
    const protocol::NewOrder order{
        id, 0, 100, 5, core::Side::Buy, core::OrderType::Limit, core::TimeInForce::GTC};
    return protocol::encode(order, /*seq=*/42);
}

// Feed bytes to a throwaway session (symbol AAPL registered); used only to prove no crash.
void feed_fresh_session(std::span<const std::byte> bytes) {
    engine::MatchingEngine eng;
    eng.register_symbol("AAPL");
    gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};
    gateway::Session session{gw};
    static_cast<void>(session.on_bytes(bytes));
}

} // namespace

// --- Layer 1: uniform-random hostile input -------------------------------------------------
// Random buffers almost never form a valid header, so this mainly exercises the framing /
// header-rejection path. Run under `make asan` it proves that layer is bounds-safe.

TEST_CASE("decoders never crash on uniform-random bytes", "[fuzz]") {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        const auto buf = random_bytes(rng, rng() % 64);
        const std::span<const std::byte> in{buf};
        static_cast<void>(protocol::decode_header(in));
        static_cast<void>(protocol::decode_new_order(in));
        static_cast<void>(protocol::decode_cancel_order(in));
        static_cast<void>(protocol::decode_heartbeat(in));
        static_cast<void>(protocol::decode_ack(in));
        static_cast<void>(protocol::decode_reject(in));
        static_cast<void>(protocol::decode_fill(in));
        static_cast<void>(feed::decode_market_data(in));
    }
    SUCCEED("no crash on uniform-random decoder input");
}

TEST_CASE("gateway session never crashes on uniform-random bytes", "[fuzz]") {
    std::mt19937_64 rng(999);
    for (int i = 0; i < 5000; ++i) {
        feed_fresh_session(random_bytes(rng, rng() % 80));
    }
    SUCCEED("no crash on uniform-random session input");
}

// --- Layer 2: structure-aware (valid header, random body) --------------------------------
// A valid header (correct version, known type, matching body_len) forces the random bytes
// past header validation into the body decoder. The result can therefore only be a body-
// level outcome (success or InvalidEnumValue), never a header error -- which proves the body
// parser is actually reached.

TEST_CASE("structure-aware NewOrder fuzzing reaches the body decoder", "[fuzz]") {
    std::mt19937_64 rng(0x5EED);
    for (int i = 0; i < 20000; ++i) {
        auto frame = valid_new_order_frame(/*id=*/1);
        for (std::size_t b = protocol::kHeaderSize; b < frame.size(); ++b) {
            frame[b] = static_cast<std::byte>(static_cast<std::uint8_t>(rng()));
        }
        const auto result = protocol::decode_new_order(frame);
        REQUIRE((result.ok() || result.error == DecodeError::InvalidEnumValue));
        feed_fresh_session(frame);
    }
    SUCCEED("structure-aware NewOrder input is body-decoded without crashing");
}

TEST_CASE("structure-aware market-data fuzzing reaches the body decoder", "[fuzz]") {
    std::mt19937_64 rng(0xFEED);
    const feed::MdTrade seed_trade{/*md_seq=*/7, /*symbol=*/2, /*price=*/12345, /*quantity=*/10};
    for (int i = 0; i < 20000; ++i) {
        auto frame = feed::encode(seed_trade);
        for (std::size_t b = protocol::kHeaderSize; b < frame.size(); ++b) {
            frame[b] = static_cast<std::byte>(static_cast<std::uint8_t>(rng()));
        }
        // MdTrade has no enum fields, so a valid header + full body always decodes.
        REQUIRE(feed::decode_md_trade(frame).ok());
        REQUIRE(feed::decode_market_data(frame).has_value());
    }
    SUCCEED("structure-aware MD input is body-decoded without crashing");
}

// --- Layer 3: mutated known-good frames ----------------------------------------------------
// Start from a valid frame and corrupt it in targeted, deterministic ways. Some corruptions
// have an assertable outcome; all must be crash/UB-free through both the decoder and session.

TEST_CASE("mutated valid frames never crash and reject deterministically", "[fuzz]") {
    const auto good = valid_new_order_frame(/*id=*/1);
    std::mt19937_64 rng(7);

    // Single-byte flips at every offset, several random values each.
    for (std::size_t pos = 0; pos < good.size(); ++pos) {
        for (int k = 0; k < 8; ++k) {
            auto m = good;
            m[pos] = static_cast<std::byte>(static_cast<std::uint8_t>(rng()));
            static_cast<void>(protocol::decode_new_order(m));
            feed_fresh_session(m);
        }
    }

    // Corrupting an enum byte (side/type/tif at body offsets 24/25/26) -> InvalidEnumValue.
    for (const std::size_t off :
         {protocol::kHeaderSize + 24, protocol::kHeaderSize + 25, protocol::kHeaderSize + 26}) {
        auto m = good;
        m[off] = std::byte{0xFF};
        REQUIRE(protocol::decode_new_order(m).error == DecodeError::InvalidEnumValue);
    }

    // Corrupting the body_len field (offset 4) to a huge value -> header-level rejection.
    {
        auto m = good;
        m[4] = std::byte{0xFF};
        const auto e = protocol::decode_new_order(m).error;
        REQUIRE((e == DecodeError::BodyTooLarge || e == DecodeError::BodyLengthMismatch ||
                 e == DecodeError::Truncated));
    }

    // The sequence-number bytes (offset 8..15) are carried, not validated -> still decodes.
    {
        auto m = good;
        m[8] = std::byte{0xAB};
        m[15] = std::byte{0xCD};
        REQUIRE(protocol::decode_new_order(m).ok());
    }

    // Trailing garbage after a complete frame.
    {
        auto m = good;
        m.push_back(std::byte{0x00});
        m.push_back(std::byte{0xFF});
        static_cast<void>(protocol::decode_new_order(m));
        feed_fresh_session(m);
    }

    // Truncation at every length.
    for (std::size_t len = 0; len <= good.size(); ++len) {
        const std::vector<std::byte> t(good.begin(),
                                       good.begin() + static_cast<std::ptrdiff_t>(len));
        static_cast<void>(protocol::decode_new_order(t));
        feed_fresh_session(t);
    }

    SUCCEED("mutated valid frames are crash-free with deterministic rejects");
}

// --- Layer 4: valid-frame reassembly through the Session -----------------------------------
// Split a valid frame across arbitrary small chunks and assert the session reassembles it:
// it never logs out, withholds any response until the frame is complete, and produces a
// response byte-identical to delivering the frame whole.

TEST_CASE("a valid frame reassembles across arbitrary chunk boundaries", "[fuzz]") {
    const auto frame = valid_new_order_frame(/*id=*/1);

    std::vector<std::byte> whole_response;
    {
        engine::MatchingEngine eng;
        eng.register_symbol("AAPL");
        gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};
        gateway::Session session{gw};
        whole_response = session.on_bytes(frame);
    }
    REQUIRE_FALSE(whole_response.empty()); // accepted order -> Ack

    engine::MatchingEngine eng;
    eng.register_symbol("AAPL");
    gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};
    gateway::Session session{gw};
    std::mt19937_64 rng(123);
    std::vector<std::byte> chunked_response;
    std::size_t fed = 0;
    while (fed < frame.size()) {
        const std::size_t n = std::min<std::size_t>(1 + (rng() % 7), frame.size() - fed);
        auto out = session.on_bytes(std::span<const std::byte>(frame.data() + fed, n));
        fed += n;
        REQUIRE_FALSE(session.logged_out());
        if (fed < frame.size()) {
            REQUIRE(out.empty()); // no response until the whole frame has arrived
        }
        chunked_response.insert(chunked_response.end(), out.begin(), out.end());
    }
    REQUIRE(chunked_response == whole_response);
}

TEST_CASE("two coalesced valid frames are reassembled in order", "[fuzz]") {
    auto both = valid_new_order_frame(/*id=*/1);
    const auto second = valid_new_order_frame(/*id=*/2);
    both.insert(both.end(), second.begin(), second.end());

    std::vector<std::byte> whole_response;
    {
        engine::MatchingEngine eng;
        eng.register_symbol("AAPL");
        gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};
        gateway::Session session{gw};
        whole_response = session.on_bytes(both);
    }
    REQUIRE_FALSE(whole_response.empty()); // two accepted orders -> two Acks

    engine::MatchingEngine eng;
    eng.register_symbol("AAPL");
    gateway::OrderGateway gw{eng, gateway::RiskConfig{1000, 1'000'000}};
    gateway::Session session{gw};
    std::mt19937_64 rng(456);
    std::vector<std::byte> chunked_response;
    std::size_t fed = 0;
    while (fed < both.size()) {
        const std::size_t n = std::min<std::size_t>(1 + (rng() % 7), both.size() - fed);
        auto out = session.on_bytes(std::span<const std::byte>(both.data() + fed, n));
        fed += n;
        REQUIRE_FALSE(session.logged_out());
        chunked_response.insert(chunked_response.end(), out.begin(), out.end());
    }
    REQUIRE(chunked_response == whole_response); // both processed, in order
}

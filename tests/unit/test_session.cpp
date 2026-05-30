#include "qsl/gateway/session.hpp"
#include "qsl/protocol/codec.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <vector>

using namespace qsl::gateway;
using namespace qsl::protocol;

namespace {
std::vector<std::vector<std::byte>> split_frames(std::span<const std::byte> bytes) {
    std::vector<std::vector<std::byte>> frames;
    std::size_t off = 0;
    while (off + kHeaderSize <= bytes.size()) {
        const auto header = decode_header(bytes.subspan(off));
        if (header.error != DecodeError::None) {
            break;
        }
        const std::size_t total = kHeaderSize + header.value.body_len;
        if (off + total > bytes.size()) {
            break;
        }
        const auto frame = bytes.subspan(off, total);
        frames.emplace_back(frame.begin(), frame.end());
        off += total;
    }
    return frames;
}

NewOrder limit(OrderId id, Side side, Price price, Quantity qty) {
    return NewOrder{id, 0, price, qty, side, OrderType::Limit, TimeInForce::GTC};
}
} // namespace

TEST_CASE("an accepted limit order yields an Ack", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    const auto out = session.on_bytes(encode(limit(1, Side::Buy, 100, 5), 1));
    REQUIRE_FALSE(session.logged_out());
    const auto ack = decode_ack(out);
    REQUIRE(ack.ok());
    REQUIRE(ack.value.order_id == 1);
}

TEST_CASE("a crossing order yields an Ack then a Fill", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    static_cast<void>(session.on_bytes(encode(limit(1, Side::Sell, 100, 5), 1))); // rests
    const auto out = session.on_bytes(encode(limit(2, Side::Buy, 100, 5), 2));    // crosses

    const auto frames = split_frames(out);
    REQUIRE(frames.size() == 2);
    REQUIRE(decode_header(frames[0]).value.type == MsgType::Ack);
    REQUIRE(decode_header(frames[1]).value.type == MsgType::Fill);
    const auto fill = decode_fill(frames[1]);
    REQUIRE(fill.value.taker_id == 2);
    REQUIRE(fill.value.maker_id == 1);
    REQUIRE(fill.value.price == 100);
    REQUIRE(fill.value.quantity == 5);
}

TEST_CASE("a market order crosses resting liquidity", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    static_cast<void>(session.on_bytes(encode(limit(1, Side::Sell, 100, 5), 1)));
    const NewOrder mkt{2, 0, 0, 5, Side::Buy, OrderType::Market, TimeInForce::GTC};
    const auto frames = split_frames(session.on_bytes(encode(mkt, 2)));
    REQUIRE(frames.size() == 2);
    REQUIRE(decode_header(frames[1]).value.type == MsgType::Fill);
    REQUIRE(decode_fill(frames[1]).value.maker_id == 1);
}

TEST_CASE("a cancel order through the session yields an Ack", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    static_cast<void>(session.on_bytes(encode(limit(1, Side::Buy, 100, 5), 1)));
    const auto ack = decode_ack(session.on_bytes(encode(CancelOrder{1, 0}, 2)));
    REQUIRE(ack.ok());
    REQUIRE(ack.value.order_id == 1);
    REQUIRE_FALSE(session.logged_out());
    REQUIRE_FALSE(eng.best_bid(0).has_value());
}

TEST_CASE("a risk-rejected order yields a Reject and keeps the session", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    // symbol 9 is not registered -> UnknownSymbol
    const NewOrder bad{1, 9, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC};
    const auto reject = decode_reject(session.on_bytes(encode(bad, 1)));
    REQUIRE(reject.ok());
    REQUIRE(reject.value.order_id == 1);
    REQUIRE(reject.value.reason == RejectReason::UnknownSymbol);
    REQUIRE_FALSE(session.logged_out()); // a valid frame, just rejected
}

TEST_CASE("a heartbeat is acknowledged with its token", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    const auto ack = decode_heartbeat_ack(session.on_bytes(encode(Heartbeat{42})));
    REQUIRE(ack.ok());
    REQUIRE(ack.value.token == 42);
}

TEST_CASE("a malformed frame flags disconnect without crashing", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    std::array<std::byte, kHeaderSize> garbage{};
    garbage.fill(static_cast<std::byte>(0xFF)); // bad version/type
    const auto out = session.on_bytes(garbage);
    REQUIRE(session.logged_out());
    REQUIRE(out.empty());
}

TEST_CASE("a valid header with an undecodable body flags disconnect", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    auto frame = encode(limit(1, Side::Buy, 100, 5), 1);
    frame[kHeaderSize + 24] = static_cast<std::byte>(99); // invalid Side byte
    const auto out = session.on_bytes(frame);
    REQUIRE(session.logged_out());
    REQUIRE(out.empty());
}

TEST_CASE("an unexpected message type flags disconnect", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    const auto out = session.on_bytes(encode(Ack{1, 1}));
    REQUIRE(session.logged_out());
    REQUIRE(out.empty());
}

TEST_CASE("a frame split across reads is buffered until complete", "[session]") {
    MatchingEngine eng;
    eng.register_symbol("AAPL");
    OrderGateway gw{eng, RiskConfig{1000, 1'000'000}};
    Session session{gw};

    const auto frame = encode(limit(1, Side::Buy, 100, 5), 1);
    const std::size_t half = frame.size() / 2;
    const auto out1 = session.on_bytes(std::span<const std::byte>(frame.data(), half));
    REQUIRE(out1.empty()); // incomplete -> no response yet
    REQUIRE_FALSE(session.logged_out());

    const auto out2 =
        session.on_bytes(std::span<const std::byte>(frame.data() + half, frame.size() - half));
    REQUIRE(decode_header(out2).value.type == MsgType::Ack);
}

#include "qsl/protocol/codec.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

using namespace qsl::protocol;

namespace {

NewOrder sample_new_order() {
    return NewOrder{/*order_id=*/1, /*symbol=*/2,     /*price=*/12345, /*quantity=*/10,
                    Side::Buy,      OrderType::Limit, TimeInForce::GTC};
}

std::span<const std::byte> as_span(const std::vector<std::byte> &v) {
    return {v.data(), v.size()};
}

inline constexpr std::size_t kNewOrderSideOffset = 24;
inline constexpr std::size_t kNewOrderOrderTypeOffset = 25;
inline constexpr std::size_t kNewOrderTifOffset = 26;

} // namespace

TEST_CASE("NewOrder encode/decode round-trips", "[protocol]") {
    const NewOrder in = sample_new_order();
    const std::vector<std::byte> frame = encode(in, /*seq=*/7);

    const auto hdr = decode_header(as_span(frame));
    REQUIRE(hdr.ok());
    REQUIRE(hdr.value.type == MsgType::NewOrder);
    REQUIRE(hdr.value.version == kProtocolVersion);
    REQUIRE(hdr.value.body_len == kNewOrderBodySize);
    REQUIRE(hdr.value.seq_no == 7);

    const auto out = decode_new_order(as_span(frame));
    REQUIRE(out.ok());
    REQUIRE(out.value.order_id == in.order_id);
    REQUIRE(out.value.symbol == in.symbol);
    REQUIRE(out.value.price == in.price);
    REQUIRE(out.value.quantity == in.quantity);
    REQUIRE(out.value.side == in.side);
    REQUIRE(out.value.type == in.type);
    REQUIRE(out.value.tif == in.tif);
}

TEST_CASE("CancelOrder encode/decode round-trips", "[protocol]") {
    const CancelOrder in{/*order_id=*/42, /*symbol=*/3};
    const std::vector<std::byte> frame = encode(in, /*seq=*/99);

    const auto out = decode_cancel_order(as_span(frame));
    REQUIRE(out.ok());
    REQUIRE(out.value.order_id == 42);
    REQUIRE(out.value.symbol == 3);
}

TEST_CASE("truncated header rejects deterministically", "[protocol]") {
    const std::array<std::byte, 8> tooShort{};
    const auto hdr = decode_header(std::span<const std::byte>{tooShort.data(), tooShort.size()});
    REQUIRE_FALSE(hdr.ok());
    REQUIRE(hdr.error == DecodeError::Truncated);
}

TEST_CASE("unsupported version rejects", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[3] = static_cast<std::byte>(2); // version 1 -> 2
    const auto hdr = decode_header(as_span(frame));
    REQUIRE(hdr.error == DecodeError::UnsupportedVersion);
}

TEST_CASE("unknown message type rejects", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[1] = static_cast<std::byte>(99); // type 1 -> 99
    const auto hdr = decode_header(as_span(frame));
    REQUIRE(hdr.error == DecodeError::UnknownType);
}

TEST_CASE("body length mismatch rejects", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[7] = static_cast<std::byte>(99); // body_len 27 -> 99 (still <= kMaxBodyLen)
    const auto out = decode_new_order(as_span(frame));
    REQUIRE(out.error == DecodeError::BodyLengthMismatch);
}

TEST_CASE("oversized declared body rejects", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[6] = static_cast<std::byte>(0x13); // body_len -> 0x1388 = 5000 > kMaxBodyLen
    frame[7] = static_cast<std::byte>(0x88);
    const auto hdr = decode_header(as_span(frame));
    REQUIRE(hdr.error == DecodeError::BodyTooLarge);
}

TEST_CASE("truncated body rejects", "[protocol]") {
    const std::vector<std::byte> frame = encode(sample_new_order(), 1);
    const std::span<const std::byte> partial{frame.data(), kHeaderSize + 10};
    const auto out = decode_new_order(partial);
    REQUIRE(out.error == DecodeError::Truncated);
}

TEST_CASE("decoding a frame as the wrong type rejects", "[protocol]") {
    const std::vector<std::byte> frame = encode(sample_new_order(), 1);
    const auto out = decode_cancel_order(as_span(frame));
    REQUIRE(out.error == DecodeError::UnknownType);
}

TEST_CASE("NewOrder rejects invalid side byte", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[kHeaderSize + kNewOrderSideOffset] = static_cast<std::byte>(99);

    const auto out = decode_new_order(as_span(frame));
    REQUIRE_FALSE(out.ok());
    REQUIRE(out.error == DecodeError::InvalidEnumValue);
}

TEST_CASE("NewOrder rejects invalid order type byte", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[kHeaderSize + kNewOrderOrderTypeOffset] = static_cast<std::byte>(99);

    const auto out = decode_new_order(as_span(frame));
    REQUIRE_FALSE(out.ok());
    REQUIRE(out.error == DecodeError::InvalidEnumValue);
}

TEST_CASE("NewOrder rejects invalid time-in-force byte", "[protocol]") {
    std::vector<std::byte> frame = encode(sample_new_order(), 1);
    frame[kHeaderSize + kNewOrderTifOffset] = static_cast<std::byte>(99);

    const auto out = decode_new_order(as_span(frame));
    REQUIRE_FALSE(out.ok());
    REQUIRE(out.error == DecodeError::InvalidEnumValue);
}

TEST_CASE("deterministic byte fixture pins the wire format", "[protocol]") {
    const std::vector<std::byte> frame = encode(sample_new_order(), /*seq=*/7);

    // Big-endian: header(16) + NewOrder body(27) = 43 bytes.
    const std::array<std::uint8_t, 43> expected{
        0x00, 0x01,                                     // type = NewOrder
        0x00, 0x01,                                     // version = 1
        0x00, 0x00, 0x00, 0x1B,                         // body_len = 27
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, // seq_no = 7
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // order_id = 1
        0x00, 0x00, 0x00, 0x02,                         // symbol = 2
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // price = 12345
        0x00, 0x00, 0x00, 0x0A,                         // quantity = 10
        0x00,                                           // side = Buy
        0x00,                                           // type = Limit
        0x00,                                           // tif = GTC
    };

    REQUIRE(frame.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(std::to_integer<std::uint8_t>(frame[i]) == expected[i]);
    }
}

TEST_CASE("decode errors stringify deterministically", "[protocol]") {
    REQUIRE(std::string_view{to_string(DecodeError::None)} == "None");
    REQUIRE(std::string_view{to_string(DecodeError::Truncated)} == "Truncated");
    REQUIRE(std::string_view{to_string(DecodeError::UnsupportedVersion)} == "UnsupportedVersion");
    REQUIRE(std::string_view{to_string(DecodeError::UnknownType)} == "UnknownType");
    REQUIRE(std::string_view{to_string(DecodeError::BodyTooLarge)} == "BodyTooLarge");
    REQUIRE(std::string_view{to_string(DecodeError::BodyLengthMismatch)} == "BodyLengthMismatch");
    REQUIRE(std::string_view{to_string(DecodeError::InvalidEnumValue)} == "InvalidEnumValue");
    REQUIRE(std::string_view{to_string(static_cast<DecodeError>(255))} == "Unknown");
}

TEST_CASE("signed price round-trips including int64 extremes", "[protocol]") {
    for (const Price p :
         {Price{-1}, std::numeric_limits<Price>::min(), std::numeric_limits<Price>::max()}) {
        NewOrder in = sample_new_order();
        in.price = p;
        const std::vector<std::byte> frame = encode(in, /*seq=*/5);
        const auto out = decode_new_order(as_span(frame));
        REQUIRE(out.ok());
        REQUIRE(out.value.price == p);
    }
}

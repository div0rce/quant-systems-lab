#include "qsl/protocol/codec.hpp"
#include "qsl/protocol/fix.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace qsl::protocol;

namespace {

constexpr char SOH = '\x01';

NewOrder sample_new_order() {
    return NewOrder{/*order_id=*/1, /*symbol=*/2,     /*price=*/12345, /*quantity=*/10,
                    Side::Buy,      OrderType::Limit, TimeInForce::GTC};
}

// Build a complete FIX message from a body (the fields from tag 35 onward),
// computing BodyLength (tag 9) and the mod-256 CheckSum (tag 10). Lets a test
// construct messages with missing/invalid body fields that encode() never emits.
std::string wrap(const std::string &body) {
    std::string head = "8=";
    head += std::string(fix::kBeginString) + SOH;
    head += "9=" + std::to_string(body.size()) + SOH;
    head += body;
    unsigned sum = 0;
    for (const char c : head) {
        sum += static_cast<unsigned char>(c);
    }
    const unsigned mod = sum % 256u;
    char cs[4];
    cs[0] = static_cast<char>('0' + ((mod / 100) % 10));
    cs[1] = static_cast<char>('0' + ((mod / 10) % 10));
    cs[2] = static_cast<char>('0' + (mod % 10));
    cs[3] = '\0';
    head += "10=";
    head += cs;
    head += SOH;
    return head;
}

std::string field(unsigned tag, std::string_view value) {
    return std::to_string(tag) + "=" + std::string(value) + SOH;
}

void require_same(const NewOrder &a, const NewOrder &b) {
    REQUIRE(a.order_id == b.order_id);
    REQUIRE(a.symbol == b.symbol);
    REQUIRE(a.price == b.price);
    REQUIRE(a.quantity == b.quantity);
    REQUIRE(a.side == b.side);
    REQUIRE(a.type == b.type);
    REQUIRE(a.tif == b.tif);
}

} // namespace

TEST_CASE("FIX NewOrder encode/decode round-trips", "[fix]") {
    const NewOrder in = sample_new_order();
    const std::string msg = fix::encode(in, /*seq=*/7);

    const auto type = fix::peek_msg_type(msg);
    REQUIRE(type.ok());
    REQUIRE(type.value == fix::kMsgNewOrderSingle);

    const auto seq = fix::peek_seq(msg);
    REQUIRE(seq.ok());
    REQUIRE(seq.value == 7);

    const auto out = fix::decode_new_order(msg);
    REQUIRE(out.ok());
    require_same(out.value, in);
}

TEST_CASE("FIX CancelOrder encode/decode round-trips", "[fix]") {
    const CancelOrder in{/*order_id=*/42, /*symbol=*/3};
    const std::string msg = fix::encode(in, /*seq=*/99);

    REQUIRE(fix::peek_msg_type(msg).value == fix::kMsgOrderCancelRequest);
    REQUIRE(fix::peek_seq(msg).value == 99);

    const auto out = fix::decode_cancel_order(msg);
    REQUIRE(out.ok());
    REQUIRE(out.value.order_id == 42);
    REQUIRE(out.value.symbol == 3);
}

TEST_CASE("FIX and binary codecs decode to identical NewOrder structs", "[fix]") {
    // The strong invariant: two independent wire formats, one internal model.
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const OrderType type : {OrderType::Limit, OrderType::Market}) {
            for (const TimeInForce tif : {TimeInForce::GTC, TimeInForce::IOC}) {
                NewOrder in = sample_new_order();
                in.side = side;
                in.type = type;
                in.tif = tif;

                const std::vector<std::byte> bin = encode(in, /*seq=*/7);
                const std::string fixmsg = fix::encode(in, /*seq=*/7);

                const auto bin_out = decode_new_order({bin.data(), bin.size()});
                const auto fix_out = fix::decode_new_order(fixmsg);
                REQUIRE(bin_out.ok());
                REQUIRE(fix_out.ok());
                require_same(bin_out.value, fix_out.value);
            }
        }
    }
}

TEST_CASE("FIX side/ord-type/tif codes map both directions", "[fix]") {
    NewOrder in = sample_new_order();
    in.side = Side::Sell;
    in.type = OrderType::Market;
    in.tif = TimeInForce::IOC;
    const std::string msg = fix::encode(in, 1);
    // 54=2 (Sell), 40=1 (Market), 59=3 (IOC).
    REQUIRE(msg.find(field(54, "2")) != std::string::npos);
    REQUIRE(msg.find(field(40, "1")) != std::string::npos);
    REQUIRE(msg.find(field(59, "3")) != std::string::npos);

    const auto out = fix::decode_new_order(msg);
    REQUIRE(out.ok());
    require_same(out.value, in);
}

TEST_CASE("FIX deterministic fixture pins the wire format", "[fix]") {
    const std::string msg = fix::encode(sample_new_order(), /*seq=*/7);
    // Built with explicit SOH so the byte sequence (and the pinned BodyLength 50
    // and CheckSum 164) are unambiguous — a "\x01..." literal would greedily
    // swallow the following digits into one hex escape.
    const std::string S(1, SOH);
    const std::string expected = "8=FIX.4.2" + S + "9=50" + S + "35=D" + S + "34=7" + S + "11=1" +
                                 S + "55=2" + S + "54=1" + S + "38=10" + S + "40=2" + S +
                                 "44=12345" + S + "59=1" + S + "10=164" + S;
    REQUIRE(msg == expected);
}

TEST_CASE("FIX malformed framing rejects deterministically", "[fix]") {
    REQUIRE(fix::decode_new_order("").error == fix::FixError::Malformed);
    REQUIRE(fix::decode_new_order("not fix at all").error == fix::FixError::Malformed);
    // A field with no '=' before its SOH.
    REQUIRE(fix::decode_new_order(std::string("8=FIX.4.2") + SOH + "garbage" + SOH).error ==
            fix::FixError::Malformed);
    // A non-numeric tag.
    REQUIRE(fix::decode_new_order(std::string("8=FIX.4.2") + SOH + "x=1" + SOH).error ==
            fix::FixError::Malformed);
    // Last field is not the checksum (tag 10).
    REQUIRE(fix::decode_new_order(std::string("8=FIX.4.2") + SOH + "9=0" + SOH).error ==
            fix::FixError::Malformed);
}

TEST_CASE("FIX oversized message rejects", "[fix]") {
    std::string body = field(35, "D");
    body += field(34, "1");
    body += "55=";
    body += std::string(fix::kMaxMessageLen, '9');
    body += SOH;
    REQUIRE(fix::decode_new_order(wrap(body)).error == fix::FixError::Malformed);
}

TEST_CASE("FIX unsupported BeginString rejects", "[fix]") {
    std::string msg = fix::encode(sample_new_order(), 1);
    const auto pos = msg.find("FIX.4.2");
    REQUIRE(pos != std::string::npos);
    msg.replace(pos, 7, "FIX.4.4"); // same width keeps BodyLength valid
    const auto out = fix::decode_new_order(msg);
    REQUIRE(out.error == fix::FixError::UnsupportedBeginString);
}

TEST_CASE("FIX unknown / wrong message type rejects", "[fix]") {
    // An unknown MsgType.
    const std::string unknown = wrap(field(35, "X") + field(34, "1"));
    REQUIRE(fix::peek_msg_type(unknown).value == 'X');
    REQUIRE(fix::decode_new_order(unknown).error == fix::FixError::UnknownMsgType);

    // A valid NewOrder decoded as a cancel rejects on type.
    const std::string neworder = fix::encode(sample_new_order(), 1);
    REQUIRE(fix::decode_cancel_order(neworder).error == fix::FixError::UnknownMsgType);
}

TEST_CASE("FIX body-length mismatch rejects", "[fix]") {
    std::string msg = fix::encode(sample_new_order(), 1);
    const auto pos = msg.find("9=50");
    REQUIRE(pos != std::string::npos);
    msg[pos + 3] = '1'; // declared 50 -> 51, actual body unchanged
    REQUIRE(fix::decode_new_order(msg).error == fix::FixError::BodyLengthMismatch);
}

TEST_CASE("FIX checksum mismatch rejects", "[fix]") {
    std::string msg = fix::encode(sample_new_order(), 1);
    REQUIRE(msg.size() >= 4);
    char &last_digit = msg[msg.size() - 2]; // the final digit before the trailing SOH
    last_digit = (last_digit == '9') ? '0' : static_cast<char>(last_digit + 1);
    REQUIRE(fix::decode_new_order(msg).error == fix::FixError::ChecksumMismatch);
}

TEST_CASE("FIX missing required field rejects", "[fix]") {
    // A NewOrder body lacking Symbol (tag 55).
    std::string body = field(35, "D") + field(34, "1") + field(11, "1") + field(54, "1") +
                       field(38, "10") + field(40, "2") + field(44, "100") + field(59, "1");
    REQUIRE(fix::decode_new_order(wrap(body)).error == fix::FixError::MissingField);
}

TEST_CASE("FIX cancel without required ClOrdID rejects", "[fix]") {
    // OrderCancelRequest lacking ClOrdID (tag 11), which FIX requires.
    std::string body = field(35, "F") + field(34, "1") + field(41, "42") + field(55, "3");
    REQUIRE(fix::decode_cancel_order(wrap(body)).error == fix::FixError::MissingField);
}

TEST_CASE("FIX invalid integer field rejects", "[fix]") {
    std::string body = field(35, "D") + field(34, "1") + field(11, "1") + field(55, "2") +
                       field(54, "1") + field(38, "abc") + field(40, "2") + field(44, "100") +
                       field(59, "1");
    REQUIRE(fix::decode_new_order(wrap(body)).error == fix::FixError::InvalidField);
}

TEST_CASE("FIX invalid enum code rejects", "[fix]") {
    std::string body = field(35, "D") + field(34, "1") + field(11, "1") + field(55, "2") +
                       field(54, "9") + field(38, "10") + field(40, "2") + field(44, "100") +
                       field(59, "1");
    REQUIRE(fix::decode_new_order(wrap(body)).error == fix::FixError::InvalidEnumValue);
}

TEST_CASE("FIX signed price round-trips including int64 extremes", "[fix]") {
    for (const Price p :
         {Price{-1}, std::numeric_limits<Price>::min(), std::numeric_limits<Price>::max()}) {
        NewOrder in = sample_new_order();
        in.price = p;
        const auto out = fix::decode_new_order(fix::encode(in, /*seq=*/5));
        REQUIRE(out.ok());
        REQUIRE(out.value.price == p);
    }
}

TEST_CASE("FIX overflowing a field reports OutOfRange", "[fix]") {
    // Symbol (tag 55) is uint32; a value past its max is OutOfRange, not Invalid.
    std::string body = field(35, "D") + field(34, "1") + field(11, "1") + field(55, "4294967296") +
                       field(54, "1") + field(38, "10") + field(40, "2") + field(44, "100") +
                       field(59, "1");
    REQUIRE(fix::decode_new_order(wrap(body)).error == fix::FixError::OutOfRange);
}

TEST_CASE("FIX large order id and seq round-trip", "[fix]") {
    NewOrder in = sample_new_order();
    in.order_id = std::numeric_limits<OrderId>::max();
    const SeqNo seq = std::numeric_limits<SeqNo>::max();
    const std::string msg = fix::encode(in, seq);
    REQUIRE(fix::peek_seq(msg).value == seq);
    REQUIRE(fix::decode_new_order(msg).value.order_id == in.order_id);
}

TEST_CASE("FIX errors stringify deterministically", "[fix]") {
    using fix::FixError;
    using fix::to_string;
    REQUIRE(std::string_view{to_string(FixError::None)} == "None");
    REQUIRE(std::string_view{to_string(FixError::Malformed)} == "Malformed");
    REQUIRE(std::string_view{to_string(FixError::UnsupportedBeginString)} ==
            "UnsupportedBeginString");
    REQUIRE(std::string_view{to_string(FixError::UnknownMsgType)} == "UnknownMsgType");
    REQUIRE(std::string_view{to_string(FixError::MissingField)} == "MissingField");
    REQUIRE(std::string_view{to_string(FixError::InvalidField)} == "InvalidField");
    REQUIRE(std::string_view{to_string(FixError::BodyLengthMismatch)} == "BodyLengthMismatch");
    REQUIRE(std::string_view{to_string(FixError::ChecksumMismatch)} == "ChecksumMismatch");
    REQUIRE(std::string_view{to_string(FixError::InvalidEnumValue)} == "InvalidEnumValue");
    REQUIRE(std::string_view{to_string(FixError::OutOfRange)} == "OutOfRange");
    REQUIRE(std::string_view{to_string(static_cast<FixError>(255))} == "Unknown");
}

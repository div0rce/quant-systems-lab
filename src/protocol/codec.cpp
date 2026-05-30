#include "qsl/protocol/codec.hpp"

#include "qsl/protocol/endian.hpp"

namespace qsl::protocol {

void write_header(std::byte *p, const MessageHeader &h) noexcept {
    store_be<std::uint16_t>(p + 0, static_cast<std::uint16_t>(h.type));
    store_be<std::uint16_t>(p + 2, h.version);
    store_be<std::uint32_t>(p + 4, h.body_len);
    store_be<std::uint64_t>(p + 8, h.seq_no);
}

std::vector<std::byte> encode(const NewOrder &msg, SeqNo seq) {
    std::vector<std::byte> buf(kHeaderSize + kNewOrderBodySize);
    write_header(buf.data(), MessageHeader{MsgType::NewOrder, kProtocolVersion,
                                           static_cast<std::uint32_t>(kNewOrderBodySize), seq});
    std::byte *b = buf.data() + kHeaderSize;
    store_be<std::uint64_t>(b + 0, msg.order_id);
    store_be<std::uint32_t>(b + 8, msg.symbol);
    store_be<std::uint64_t>(b + 12, static_cast<std::uint64_t>(msg.price));
    store_be<std::uint32_t>(b + 20, msg.quantity);
    b[24] = static_cast<std::byte>(static_cast<std::uint8_t>(msg.side));
    b[25] = static_cast<std::byte>(static_cast<std::uint8_t>(msg.type));
    b[26] = static_cast<std::byte>(static_cast<std::uint8_t>(msg.tif));
    return buf;
}

std::vector<std::byte> encode(const CancelOrder &msg, SeqNo seq) {
    std::vector<std::byte> buf(kHeaderSize + kCancelOrderBodySize);
    write_header(buf.data(), MessageHeader{MsgType::CancelOrder, kProtocolVersion,
                                           static_cast<std::uint32_t>(kCancelOrderBodySize), seq});
    std::byte *b = buf.data() + kHeaderSize;
    store_be<std::uint64_t>(b + 0, msg.order_id);
    store_be<std::uint32_t>(b + 8, msg.symbol);
    return buf;
}

DecodeResult<MessageHeader> decode_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kHeaderSize) {
        return {DecodeError::Truncated, {}};
    }
    const std::byte *p = bytes.data();
    MessageHeader h{};
    h.type = static_cast<MsgType>(load_be<std::uint16_t>(p + 0));
    h.version = load_be<std::uint16_t>(p + 2);
    h.body_len = load_be<std::uint32_t>(p + 4);
    h.seq_no = load_be<std::uint64_t>(p + 8);

    if (h.version != kProtocolVersion) {
        return {DecodeError::UnsupportedVersion, h};
    }
    if (!is_known(h.type)) {
        return {DecodeError::UnknownType, h};
    }
    if (h.body_len > kMaxBodyLen) {
        return {DecodeError::BodyTooLarge, h};
    }
    return {DecodeError::None, h};
}

DecodeResult<NewOrder> decode_new_order(std::span<const std::byte> frame) noexcept {
    const auto hr = decode_header(frame);
    if (!hr.ok()) {
        return {hr.error, {}};
    }
    if (hr.value.type != MsgType::NewOrder) {
        return {DecodeError::UnknownType, {}};
    }
    if (hr.value.body_len != kNewOrderBodySize) {
        return {DecodeError::BodyLengthMismatch, {}};
    }
    if (frame.size() < kHeaderSize + kNewOrderBodySize) {
        return {DecodeError::Truncated, {}};
    }
    const std::byte *b = frame.data() + kHeaderSize;
    NewOrder msg{};
    msg.order_id = load_be<std::uint64_t>(b + 0);
    msg.symbol = load_be<std::uint32_t>(b + 8);
    msg.price = static_cast<Price>(load_be<std::uint64_t>(b + 12));
    msg.quantity = load_be<std::uint32_t>(b + 20);
    const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(b[24]));
    const auto type = static_cast<OrderType>(std::to_integer<std::uint8_t>(b[25]));
    const auto tif = static_cast<TimeInForce>(std::to_integer<std::uint8_t>(b[26]));
    if (!is_valid(side) || !is_valid(type) || !is_valid(tif)) {
        return {DecodeError::InvalidEnumValue, {}};
    }
    msg.side = side;
    msg.type = type;
    msg.tif = tif;
    return {DecodeError::None, msg};
}

DecodeResult<CancelOrder> decode_cancel_order(std::span<const std::byte> frame) noexcept {
    const auto hr = decode_header(frame);
    if (!hr.ok()) {
        return {hr.error, {}};
    }
    if (hr.value.type != MsgType::CancelOrder) {
        return {DecodeError::UnknownType, {}};
    }
    if (hr.value.body_len != kCancelOrderBodySize) {
        return {DecodeError::BodyLengthMismatch, {}};
    }
    if (frame.size() < kHeaderSize + kCancelOrderBodySize) {
        return {DecodeError::Truncated, {}};
    }
    const std::byte *b = frame.data() + kHeaderSize;
    CancelOrder msg{};
    msg.order_id = load_be<std::uint64_t>(b + 0);
    msg.symbol = load_be<std::uint32_t>(b + 8);
    return {DecodeError::None, msg};
}

namespace {

// Validate a typed frame's header (type, body length, buffer size). Returns the body
// pointer, or nullptr with `err` set.
const std::byte *validate_typed(std::span<const std::byte> frame, MsgType expected,
                                std::size_t body_size, DecodeError &err) noexcept {
    const auto hr = decode_header(frame);
    if (!hr.ok()) {
        err = hr.error;
        return nullptr;
    }
    if (hr.value.type != expected) {
        err = DecodeError::UnknownType;
        return nullptr;
    }
    if (hr.value.body_len != body_size) {
        err = DecodeError::BodyLengthMismatch;
        return nullptr;
    }
    if (frame.size() < kHeaderSize + body_size) {
        err = DecodeError::Truncated;
        return nullptr;
    }
    err = DecodeError::None;
    return frame.data() + kHeaderSize;
}

std::vector<std::byte> framed(MsgType type, std::size_t body_size) {
    std::vector<std::byte> buf(kHeaderSize + body_size);
    write_header(buf.data(),
                 MessageHeader{type, kProtocolVersion, static_cast<std::uint32_t>(body_size), 0});
    return buf;
}

} // namespace

std::vector<std::byte> encode(const Heartbeat &msg) {
    auto buf = framed(MsgType::Heartbeat, kHeartbeatBodySize);
    store_be<std::uint64_t>(buf.data() + kHeaderSize, msg.token);
    return buf;
}

std::vector<std::byte> encode(const HeartbeatAck &msg) {
    auto buf = framed(MsgType::HeartbeatAck, kHeartbeatBodySize);
    store_be<std::uint64_t>(buf.data() + kHeaderSize, msg.token);
    return buf;
}

std::vector<std::byte> encode(const Ack &msg) {
    auto buf = framed(MsgType::Ack, kAckBodySize);
    std::byte *b = buf.data() + kHeaderSize;
    store_be<std::uint64_t>(b + 0, msg.order_id);
    store_be<std::uint64_t>(b + 8, msg.seq);
    return buf;
}

std::vector<std::byte> encode(const Reject &msg) {
    auto buf = framed(MsgType::Reject, kRejectBodySize);
    std::byte *b = buf.data() + kHeaderSize;
    store_be<std::uint64_t>(b + 0, msg.order_id);
    b[8] = static_cast<std::byte>(static_cast<std::uint8_t>(msg.reason));
    return buf;
}

std::vector<std::byte> encode(const Fill &msg) {
    auto buf = framed(MsgType::Fill, kFillBodySize);
    std::byte *b = buf.data() + kHeaderSize;
    store_be<std::uint64_t>(b + 0, msg.taker_id);
    store_be<std::uint64_t>(b + 8, msg.maker_id);
    store_be<std::uint64_t>(b + 16, static_cast<std::uint64_t>(msg.price));
    store_be<std::uint32_t>(b + 24, msg.quantity);
    store_be<std::uint64_t>(b + 28, msg.seq);
    return buf;
}

DecodeResult<Heartbeat> decode_heartbeat(std::span<const std::byte> frame) noexcept {
    DecodeError err{};
    const std::byte *b = validate_typed(frame, MsgType::Heartbeat, kHeartbeatBodySize, err);
    if (b == nullptr) {
        return {err, {}};
    }
    return {DecodeError::None, Heartbeat{load_be<std::uint64_t>(b)}};
}

DecodeResult<HeartbeatAck> decode_heartbeat_ack(std::span<const std::byte> frame) noexcept {
    DecodeError err{};
    const std::byte *b = validate_typed(frame, MsgType::HeartbeatAck, kHeartbeatBodySize, err);
    if (b == nullptr) {
        return {err, {}};
    }
    return {DecodeError::None, HeartbeatAck{load_be<std::uint64_t>(b)}};
}

DecodeResult<Ack> decode_ack(std::span<const std::byte> frame) noexcept {
    DecodeError err{};
    const std::byte *b = validate_typed(frame, MsgType::Ack, kAckBodySize, err);
    if (b == nullptr) {
        return {err, {}};
    }
    return {DecodeError::None, Ack{load_be<std::uint64_t>(b + 0), load_be<std::uint64_t>(b + 8)}};
}

DecodeResult<Reject> decode_reject(std::span<const std::byte> frame) noexcept {
    DecodeError err{};
    const std::byte *b = validate_typed(frame, MsgType::Reject, kRejectBodySize, err);
    if (b == nullptr) {
        return {err, {}};
    }
    return {DecodeError::None,
            Reject{load_be<std::uint64_t>(b + 0),
                   static_cast<RejectReason>(std::to_integer<std::uint8_t>(b[8]))}};
}

DecodeResult<Fill> decode_fill(std::span<const std::byte> frame) noexcept {
    DecodeError err{};
    const std::byte *b = validate_typed(frame, MsgType::Fill, kFillBodySize, err);
    if (b == nullptr) {
        return {err, {}};
    }
    Fill msg{};
    msg.taker_id = load_be<std::uint64_t>(b + 0);
    msg.maker_id = load_be<std::uint64_t>(b + 8);
    msg.price = static_cast<Price>(load_be<std::uint64_t>(b + 16));
    msg.quantity = load_be<std::uint32_t>(b + 24);
    msg.seq = load_be<std::uint64_t>(b + 28);
    return {DecodeError::None, msg};
}

} // namespace qsl::protocol

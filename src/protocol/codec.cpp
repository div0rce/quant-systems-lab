#include "qsl/protocol/codec.hpp"

#include "qsl/protocol/endian.hpp"

namespace qsl::protocol {
namespace {

void write_header(std::byte *p, const MessageHeader &h) noexcept {
    store_be<std::uint16_t>(p + 0, static_cast<std::uint16_t>(h.type));
    store_be<std::uint16_t>(p + 2, h.version);
    store_be<std::uint32_t>(p + 4, h.body_len);
    store_be<std::uint64_t>(p + 8, h.seq_no);
}

} // namespace

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

} // namespace qsl::protocol

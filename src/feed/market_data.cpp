#include "qsl/feed/market_data.hpp"

#include "qsl/protocol/endian.hpp"
#include "qsl/protocol/messages.hpp"

namespace qsl::feed {

using protocol::DecodeError;
using protocol::kHeaderSize;
using protocol::kProtocolVersion;
using protocol::MessageHeader;
using protocol::MsgType;

namespace {
inline constexpr std::size_t kMdTradeBody = 16; // symbol(4) price(8) quantity(4)
inline constexpr std::size_t kMdTopOfBookBody =
    22; // symbol(4) bid_flag(1) bid(8) ask_flag(1) ask(8)

std::byte flag(bool present) noexcept {
    return static_cast<std::byte>(static_cast<std::uint8_t>(present ? 1 : 0));
}
} // namespace

std::vector<std::byte> encode(const MdTrade &msg) {
    std::vector<std::byte> buf(kHeaderSize + kMdTradeBody);
    protocol::write_header(buf.data(),
                           MessageHeader{MsgType::MdTrade, kProtocolVersion,
                                         static_cast<std::uint32_t>(kMdTradeBody), msg.md_seq});
    std::byte *b = buf.data() + kHeaderSize;
    protocol::store_be<std::uint32_t>(b + 0, msg.symbol);
    protocol::store_be<std::uint64_t>(b + 4, static_cast<std::uint64_t>(msg.price));
    protocol::store_be<std::uint32_t>(b + 12, msg.quantity);
    return buf;
}

std::vector<std::byte> encode(const MdTopOfBook &msg) {
    std::vector<std::byte> buf(kHeaderSize + kMdTopOfBookBody);
    protocol::write_header(buf.data(),
                           MessageHeader{MsgType::MdTopOfBook, kProtocolVersion,
                                         static_cast<std::uint32_t>(kMdTopOfBookBody), msg.md_seq});
    std::byte *b = buf.data() + kHeaderSize;
    protocol::store_be<std::uint32_t>(b + 0, msg.symbol);
    b[4] = flag(msg.best_bid.has_value());
    protocol::store_be<std::uint64_t>(b + 5, static_cast<std::uint64_t>(msg.best_bid.value_or(0)));
    b[13] = flag(msg.best_ask.has_value());
    protocol::store_be<std::uint64_t>(b + 14, static_cast<std::uint64_t>(msg.best_ask.value_or(0)));
    return buf;
}

protocol::DecodeResult<MdTrade> decode_md_trade(std::span<const std::byte> frame) {
    const auto hr = protocol::decode_header(frame);
    if (!hr.ok()) {
        return {hr.error, {}};
    }
    if (hr.value.type != MsgType::MdTrade) {
        return {DecodeError::UnknownType, {}};
    }
    if (hr.value.body_len != kMdTradeBody) {
        return {DecodeError::BodyLengthMismatch, {}};
    }
    if (frame.size() < kHeaderSize + kMdTradeBody) {
        return {DecodeError::Truncated, {}};
    }
    const std::byte *b = frame.data() + kHeaderSize;
    MdTrade msg{};
    msg.md_seq = hr.value.seq_no;
    msg.symbol = protocol::load_be<std::uint32_t>(b + 0);
    msg.price = static_cast<Price>(protocol::load_be<std::uint64_t>(b + 4));
    msg.quantity = protocol::load_be<std::uint32_t>(b + 12);
    return {DecodeError::None, msg};
}

protocol::DecodeResult<MdTopOfBook> decode_md_top_of_book(std::span<const std::byte> frame) {
    const auto hr = protocol::decode_header(frame);
    if (!hr.ok()) {
        return {hr.error, {}};
    }
    if (hr.value.type != MsgType::MdTopOfBook) {
        return {DecodeError::UnknownType, {}};
    }
    if (hr.value.body_len != kMdTopOfBookBody) {
        return {DecodeError::BodyLengthMismatch, {}};
    }
    if (frame.size() < kHeaderSize + kMdTopOfBookBody) {
        return {DecodeError::Truncated, {}};
    }
    const std::byte *b = frame.data() + kHeaderSize;
    MdTopOfBook msg{};
    msg.md_seq = hr.value.seq_no;
    msg.symbol = protocol::load_be<std::uint32_t>(b + 0);
    if (std::to_integer<std::uint8_t>(b[4]) != 0) {
        msg.best_bid = static_cast<Price>(protocol::load_be<std::uint64_t>(b + 5));
    }
    if (std::to_integer<std::uint8_t>(b[13]) != 0) {
        msg.best_ask = static_cast<Price>(protocol::load_be<std::uint64_t>(b + 14));
    }
    return {DecodeError::None, msg};
}

} // namespace qsl::feed

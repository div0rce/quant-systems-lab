#pragma once

#include "qsl/core/types.hpp"

#include <cstddef>
#include <cstdint>

namespace qsl::protocol {

using core::OrderId;
using core::OrderType;
using core::Price;
using core::Quantity;
using core::SeqNo;
using core::Side;
using core::SymbolId;
using core::TimeInForce;

// Wire constants. Big-endian fixed-width frames: a 16-byte header + a body.
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 16; // type(2) version(2) body_len(4) seq_no(8)
inline constexpr std::uint32_t kMaxBodyLen = 4096;

// Message type registry. Extended as later milestones add messages.
enum class MsgType : std::uint16_t {
    NewOrder = 1,
    CancelOrder = 2,
    MdTrade = 3,
    MdTopOfBook = 4,
};

[[nodiscard]] constexpr bool is_known(MsgType t) noexcept {
    return t == MsgType::NewOrder || t == MsgType::CancelOrder || t == MsgType::MdTrade ||
           t == MsgType::MdTopOfBook;
}

struct MessageHeader {
    MsgType type;
    std::uint16_t version;
    std::uint32_t body_len;
    SeqNo seq_no;
};

// Body layouts (explicit, no padding on the wire).
struct NewOrder {
    OrderId order_id;
    SymbolId symbol;
    Price price;
    Quantity quantity;
    Side side;
    OrderType type;
    TimeInForce tif;
};
inline constexpr std::size_t kNewOrderBodySize = 27; // 8+4+8+4+1+1+1

struct CancelOrder {
    OrderId order_id;
    SymbolId symbol;
};
inline constexpr std::size_t kCancelOrderBodySize = 12; // 8+4

// Deterministic decode outcomes for malformed/invalid frames.
enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,          // fewer bytes than the header or the declared body
    UnsupportedVersion, // header version != kProtocolVersion
    UnknownType,        // header type not in the message registry
    BodyTooLarge,       // declared body_len exceeds kMaxBodyLen
    BodyLengthMismatch, // declared body_len != the type's fixed body size
    InvalidEnumValue,   // enum byte is not valid for the target domain enum
};

[[nodiscard]] constexpr const char *to_string(DecodeError e) noexcept {
    switch (e) {
    case DecodeError::None:
        return "None";
    case DecodeError::Truncated:
        return "Truncated";
    case DecodeError::UnsupportedVersion:
        return "UnsupportedVersion";
    case DecodeError::UnknownType:
        return "UnknownType";
    case DecodeError::BodyTooLarge:
        return "BodyTooLarge";
    case DecodeError::BodyLengthMismatch:
        return "BodyLengthMismatch";
    case DecodeError::InvalidEnumValue:
        return "InvalidEnumValue";
    }
    return "Unknown";
}

} // namespace qsl::protocol

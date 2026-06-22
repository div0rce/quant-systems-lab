#pragma once

// FIX-like text protocol adapter (issue #29).
//
// A human-readable `tag=value` wire format alongside the binary codec
// (qsl/protocol/codec.hpp), mapping the same internal message structs. It is
// "FIX-like": it uses genuine FIX framing — SOH-delimited tag=value fields, the
// 8/9/35/.../10 envelope, a BodyLength (tag 9) and a mod-256 CheckSum (tag 10) —
// for the client->gateway order path: NewOrderSingle (35=D) -> NewOrder and
// OrderCancelRequest (35=F) -> CancelOrder.
//
// Deliberate, documented simplifications for a deterministic simulator (see
// docs/fix_protocol.md):
//   * Symbol (tag 55) carries the numeric SymbolId in decimal, not a ticker
//     string — the internal model keys on SymbolId.
//   * Price (tag 44) carries integer ticks, never a decimal/float, and is always
//     present (including market orders). This keeps NewOrder<->FIX a lossless
//     bijection over the internal struct, exactly like the binary codec, so a
//     binary frame and a FIX message for the same order decode to identical
//     structs. Prices are never floating point.
//
// Decoding is total and deterministic: it never throws, allocates only the
// returned string on encode, and reports every malformed input through FixError
// rather than undefined behavior — mirroring the binary codec's DecodeError
// discipline.

#include "qsl/protocol/messages.hpp"

#include <string>
#include <string_view>

namespace qsl::protocol::fix {

// FIX field separator (SOH, 0x01) and the supported BeginString (tag 8).
inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString = "FIX.4.2";
// Defensive upper bound on a single message; order messages are small.
inline constexpr std::size_t kMaxMessageLen = 1024;

// FIX MsgType (tag 35) values this adapter handles.
inline constexpr char kMsgNewOrderSingle = 'D';
inline constexpr char kMsgOrderCancelRequest = 'F';

// Deterministic decode outcomes for malformed/invalid FIX text. Extends the
// binary codec's error taxonomy with FIX-envelope-specific failures.
enum class FixError : std::uint8_t {
    None = 0,
    Malformed,              // not tag=value/SOH framed, or empty/oversized
    UnsupportedBeginString, // tag 8 != kBeginString
    UnknownMsgType,         // tag 35 absent, or not the expected message type
    MissingField,           // a required tag is absent
    InvalidField,           // a value failed integer parsing / is empty
    BodyLengthMismatch,     // tag 9 (BodyLength) != the actual body byte count
    ChecksumMismatch,       // tag 10 (CheckSum) != the computed mod-256 sum
    InvalidEnumValue,       // Side/OrdType/TimeInForce code is not recognized
    OutOfRange,             // a parsed integer does not fit its target field
};

[[nodiscard]] constexpr const char *to_string(FixError e) noexcept {
    switch (e) {
    case FixError::None:
        return "None";
    case FixError::Malformed:
        return "Malformed";
    case FixError::UnsupportedBeginString:
        return "UnsupportedBeginString";
    case FixError::UnknownMsgType:
        return "UnknownMsgType";
    case FixError::MissingField:
        return "MissingField";
    case FixError::InvalidField:
        return "InvalidField";
    case FixError::BodyLengthMismatch:
        return "BodyLengthMismatch";
    case FixError::ChecksumMismatch:
        return "ChecksumMismatch";
    case FixError::InvalidEnumValue:
        return "InvalidEnumValue";
    case FixError::OutOfRange:
        return "OutOfRange";
    }
    return "Unknown";
}

template <class T> struct FixDecodeResult {
    FixError error{FixError::None};
    T value{};

    [[nodiscard]] bool ok() const noexcept { return error == FixError::None; }
};

// Encode internal order structs to a complete FIX-like message string (a single
// allocation, framed with BeginString/BodyLength/CheckSum). `seq` is carried in
// MsgSeqNum (tag 34), mirroring the binary frame's header sequence number.
[[nodiscard]] std::string encode(const NewOrder &msg, SeqNo seq);
[[nodiscard]] std::string encode(const CancelOrder &msg, SeqNo seq);

// Decode and validate a complete FIX-like message into the internal struct.
[[nodiscard]] FixDecodeResult<NewOrder> decode_new_order(std::string_view msg) noexcept;
[[nodiscard]] FixDecodeResult<CancelOrder> decode_cancel_order(std::string_view msg) noexcept;

// Validate the FIX envelope (8/9/.../10) and return the MsgType (tag 35) so a
// dispatcher can route to the right typed decoder.
[[nodiscard]] FixDecodeResult<char> peek_msg_type(std::string_view msg) noexcept;

// Validate the envelope and return MsgSeqNum (tag 34). Useful for verifying that
// the sequence number round-trips, since the typed decoders return only the body.
[[nodiscard]] FixDecodeResult<SeqNo> peek_seq(std::string_view msg) noexcept;

} // namespace qsl::protocol::fix

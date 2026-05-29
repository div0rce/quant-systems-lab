#pragma once

#include "qsl/protocol/messages.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace qsl::protocol {

template <class T> struct DecodeResult {
    DecodeError error{DecodeError::None};
    T value{};

    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }
};

// Encode a full frame (header + body). Header carries the assigned sequence number.
[[nodiscard]] std::vector<std::byte> encode(const NewOrder &msg, SeqNo seq);
[[nodiscard]] std::vector<std::byte> encode(const CancelOrder &msg, SeqNo seq);

// Decode and validate the header only (version, type, max body length).
[[nodiscard]] DecodeResult<MessageHeader> decode_header(std::span<const std::byte> bytes) noexcept;

// Decode and validate a full typed frame.
[[nodiscard]] DecodeResult<NewOrder> decode_new_order(std::span<const std::byte> frame) noexcept;
[[nodiscard]] DecodeResult<CancelOrder>
decode_cancel_order(std::span<const std::byte> frame) noexcept;

} // namespace qsl::protocol

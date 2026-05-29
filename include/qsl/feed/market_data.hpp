#pragma once

#include "qsl/core/types.hpp"
#include "qsl/protocol/codec.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace qsl::feed {

using core::Price;
using core::Quantity;
using core::SeqNo;
using core::SymbolId;

// Market-data messages. Each carries the publisher's monotonic md sequence number.

struct MdTrade {
    SeqNo md_seq;
    SymbolId symbol;
    Price price;
    Quantity quantity;
    bool operator==(const MdTrade &) const = default;
};

struct MdTopOfBook {
    SeqNo md_seq;
    SymbolId symbol;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    bool operator==(const MdTopOfBook &) const = default;
};

using MarketDataMessage = std::variant<MdTrade, MdTopOfBook>;

[[nodiscard]] inline SeqNo md_seq_of(const MarketDataMessage &msg) noexcept {
    return std::visit([](const auto &m) noexcept { return m.md_seq; }, msg);
}

// Binary encoding over the M2 protocol framing (md_seq is carried in the header seq_no).
[[nodiscard]] std::vector<std::byte> encode(const MdTrade &msg);
[[nodiscard]] std::vector<std::byte> encode(const MdTopOfBook &msg);
[[nodiscard]] protocol::DecodeResult<MdTrade> decode_md_trade(std::span<const std::byte> frame);
[[nodiscard]] protocol::DecodeResult<MdTopOfBook>
decode_md_top_of_book(std::span<const std::byte> frame);

} // namespace qsl::feed

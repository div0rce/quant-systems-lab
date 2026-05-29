#pragma once

#include "qsl/core/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace qsl::replay {

using core::OrderId;
using core::OrderType;
using core::Price;
using core::Quantity;
using core::Side;
using core::SymbolId;
using core::TimeInForce;

// Recordable command stream. RegisterSymbol is included so a log replays standalone:
// registering the same names in the same order reproduces identical SymbolIds.
struct RegisterSymbol {
    std::string name;
    bool operator==(const RegisterSymbol &) const = default;
};
struct NewLimit {
    SymbolId symbol;
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    TimeInForce tif;
    bool operator==(const NewLimit &) const = default;
};
struct NewMarket {
    SymbolId symbol;
    OrderId id;
    Side side;
    Quantity quantity;
    bool operator==(const NewMarket &) const = default;
};
struct Cancel {
    SymbolId symbol;
    OrderId id;
    bool operator==(const Cancel &) const = default;
};
struct Modify {
    SymbolId symbol;
    OrderId id;
    Price price;
    Quantity quantity;
    bool operator==(const Modify &) const = default;
};

using Command = std::variant<RegisterSymbol, NewLimit, NewMarket, Cancel, Modify>;

// Compact serialization: a 1-byte tag followed by fixed-width fields (RegisterSymbol's
// name fills the remaining bytes). Used as the payload of a log Command record.
[[nodiscard]] std::vector<std::byte> encode_command(const Command &command);
[[nodiscard]] std::optional<Command> decode_command(std::span<const std::byte> bytes);

} // namespace qsl::replay

#pragma once

#include <cstdint>

namespace qsl::core {

/// Structured rejection codes returned by validation/risk paths.
enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownSymbol,
    InvalidPrice,
    InvalidQuantity,
    InvalidSide,
    MaxQuantityExceeded,
    MaxNotionalExceeded,
    DuplicateOrderId,
    UnknownOrder,
    StorageExhausted,
};

[[nodiscard]] constexpr const char *to_string(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None:
        return "None";
    case RejectReason::UnknownSymbol:
        return "UnknownSymbol";
    case RejectReason::InvalidPrice:
        return "InvalidPrice";
    case RejectReason::InvalidQuantity:
        return "InvalidQuantity";
    case RejectReason::InvalidSide:
        return "InvalidSide";
    case RejectReason::MaxQuantityExceeded:
        return "MaxQuantityExceeded";
    case RejectReason::MaxNotionalExceeded:
        return "MaxNotionalExceeded";
    case RejectReason::DuplicateOrderId:
        return "DuplicateOrderId";
    case RejectReason::UnknownOrder:
        return "UnknownOrder";
    case RejectReason::StorageExhausted:
        return "StorageExhausted";
    }
    return "Unknown";
}

/// Minimal success/failure result carrying a reject reason.
struct Result {
    bool ok;
    RejectReason reason;

    [[nodiscard]] static constexpr Result success() noexcept { return {true, RejectReason::None}; }

    [[nodiscard]] static constexpr Result reject(RejectReason r) noexcept { return {false, r}; }
};

} // namespace qsl::core

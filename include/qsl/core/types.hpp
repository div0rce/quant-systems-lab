#pragma once

#include <cstdint>

namespace qsl::core {

using SymbolId = std::uint32_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;
using SeqNo = std::uint64_t;

/// Smoke-test function to verify build.
[[nodiscard]] auto version() noexcept -> const char *;

} // namespace qsl::core

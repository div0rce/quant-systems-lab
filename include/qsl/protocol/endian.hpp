#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace qsl::protocol {

// Explicit big-endian (network byte order) serialization via byte shifts.
// No reinterpret_cast, no memcpy, no struct overlay, well-defined on any host.

template <class UInt> void store_be(std::byte *p, UInt v) noexcept {
    static_assert(std::is_unsigned_v<UInt>, "store_be requires an unsigned type");
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        p[sizeof(UInt) - 1 - i] = static_cast<std::byte>(static_cast<std::uint8_t>(v));
        v = static_cast<UInt>(v >> 8);
    }
}

template <class UInt> [[nodiscard]] UInt load_be(const std::byte *p) noexcept {
    static_assert(std::is_unsigned_v<UInt>, "load_be requires an unsigned type");
    UInt v = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        v = static_cast<UInt>((v << 8) | std::to_integer<UInt>(p[i]));
    }
    return v;
}

} // namespace qsl::protocol

#include "qsl/core/types.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version returns non-null", "[core]") {
    REQUIRE(qsl::core::version() != nullptr);
}

TEST_CASE("type aliases have expected sizes", "[core]") {
    STATIC_REQUIRE(sizeof(qsl::core::Price) == 8);
    STATIC_REQUIRE(sizeof(qsl::core::Quantity) == 4);
    STATIC_REQUIRE(sizeof(qsl::core::OrderId) == 8);
    STATIC_REQUIRE(sizeof(qsl::core::SeqNo) == 8);
    STATIC_REQUIRE(sizeof(qsl::core::SymbolId) == 4);
}

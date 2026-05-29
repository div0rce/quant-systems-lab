#include "qsl/core/invariants.hpp"
#include "qsl/core/result.hpp"
#include "qsl/core/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <type_traits>

using namespace qsl::core;

TEST_CASE("price type is integer, not floating point", "[types]") {
    STATIC_REQUIRE(std::is_integral_v<Price>);
    STATIC_REQUIRE(std::is_signed_v<Price>);
}

TEST_CASE("enum validity accepts defined values, rejects out-of-range casts", "[types]") {
    REQUIRE(is_valid(Side::Buy));
    REQUIRE(is_valid(Side::Sell));
    REQUIRE_FALSE(is_valid(static_cast<Side>(2)));

    REQUIRE(is_valid(OrderType::Limit));
    REQUIRE(is_valid(OrderType::Market));
    REQUIRE_FALSE(is_valid(static_cast<OrderType>(7)));

    REQUIRE(is_valid(TimeInForce::GTC));
    REQUIRE(is_valid(TimeInForce::IOC));
    REQUIRE_FALSE(is_valid(static_cast<TimeInForce>(9)));
}

TEST_CASE("enum to_string conversions are stable", "[types]") {
    REQUIRE(std::string_view{to_string(Side::Buy)} == "Buy");
    REQUIRE(std::string_view{to_string(Side::Sell)} == "Sell");
    REQUIRE(std::string_view{to_string(OrderType::Limit)} == "Limit");
    REQUIRE(std::string_view{to_string(OrderType::Market)} == "Market");
    REQUIRE(std::string_view{to_string(TimeInForce::GTC)} == "GTC");
    REQUIRE(std::string_view{to_string(TimeInForce::IOC)} == "IOC");
    REQUIRE(std::string_view{to_string(static_cast<Side>(2))} == "Unknown");
}

TEST_CASE("reject reasons stringify deterministically", "[result]") {
    REQUIRE(std::string_view{to_string(RejectReason::None)} == "None");
    REQUIRE(std::string_view{to_string(RejectReason::UnknownSymbol)} == "UnknownSymbol");
    REQUIRE(std::string_view{to_string(RejectReason::InvalidPrice)} == "InvalidPrice");
    REQUIRE(std::string_view{to_string(RejectReason::InvalidQuantity)} == "InvalidQuantity");
    REQUIRE(std::string_view{to_string(RejectReason::InvalidSide)} == "InvalidSide");
    REQUIRE(std::string_view{to_string(RejectReason::MaxQuantityExceeded)} ==
            "MaxQuantityExceeded");
    REQUIRE(std::string_view{to_string(RejectReason::MaxNotionalExceeded)} ==
            "MaxNotionalExceeded");
    REQUIRE(std::string_view{to_string(RejectReason::DuplicateOrderId)} == "DuplicateOrderId");
    REQUIRE(std::string_view{to_string(RejectReason::UnknownOrder)} == "UnknownOrder");
    REQUIRE(std::string_view{to_string(static_cast<RejectReason>(255))} == "Unknown");
}

TEST_CASE("Result carries ok/reason", "[result]") {
    constexpr Result ok = Result::success();
    STATIC_REQUIRE(ok.ok);
    STATIC_REQUIRE(ok.reason == RejectReason::None);

    constexpr Result bad = Result::reject(RejectReason::UnknownSymbol);
    STATIC_REQUIRE_FALSE(bad.ok);
    STATIC_REQUIRE(bad.reason == RejectReason::UnknownSymbol);
}

TEST_CASE("domain predicates reject invalid values deterministically", "[invariants]") {
    REQUIRE(is_valid_price(1));
    REQUIRE_FALSE(is_valid_price(0));
    REQUIRE_FALSE(is_valid_price(-100));

    REQUIRE(is_valid_quantity(1));
    REQUIRE_FALSE(is_valid_quantity(0));

    REQUIRE(validate_limit(12345, 10).ok);
    REQUIRE(validate_limit(0, 10).reason == RejectReason::InvalidPrice);
    REQUIRE(validate_limit(12345, 0).reason == RejectReason::InvalidQuantity);
}

#include "qsl/core/clock.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace qsl::core;

TEST_CASE("logical clock starts at zero and ticks monotonically", "[clock]") {
    LogicalClock clock;
    REQUIRE(clock.now() == 0);

    const Timestamp first = clock.tick();
    const Timestamp second = clock.tick();
    REQUIRE(first == 1);
    REQUIRE(second == 2);
    REQUIRE(second > first);
    REQUIRE(clock.now() == 2);
}

TEST_CASE("logical clock advances by an explicit amount", "[clock]") {
    LogicalClock clock;
    clock.advance(100);
    REQUIRE(clock.now() == 100);
    REQUIRE(clock.tick() == 101);
}

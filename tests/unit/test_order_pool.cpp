#include "qsl/memory/order_pool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>

using qsl::core::Side;
using qsl::memory::OrderPool;

TEST_CASE("order pool has deterministic capacity and exhaustion", "[order_pool]") {
    OrderPool<2> pool;
    REQUIRE(pool.capacity() == 2);
    REQUIRE(pool.available() == 2);
    REQUIRE(pool.in_use() == 0);

    auto *a = pool.try_acquire(1, Side::Buy, 100, 5);
    REQUIRE(a != nullptr);
    REQUIRE(a->id == 1);
    REQUIRE(a->side == Side::Buy);
    REQUIRE(a->price == 100);
    REQUIRE(a->quantity == 5);

    auto *b = pool.try_acquire(2, Side::Sell, 101, 7);
    REQUIRE(b != nullptr);
    REQUIRE(pool.available() == 0);
    REQUIRE(pool.in_use() == 2);

    REQUIRE(pool.try_acquire(3, Side::Buy, 99, 1) == nullptr);
    REQUIRE(pool.available() == 0);
    REQUIRE(pool.in_use() == 2);
}

TEST_CASE("order pool releases and reuses slots without heap fallback", "[order_pool]") {
    OrderPool<2> pool;

    auto *a = pool.try_acquire(1, Side::Buy, 100, 5);
    auto *b = pool.try_acquire(2, Side::Sell, 101, 7);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    REQUIRE(pool.release(a));
    REQUIRE(pool.available() == 1);
    REQUIRE(pool.in_use() == 1);

    auto *c = pool.try_acquire(3, Side::Buy, 102, 9);
    REQUIRE(c == a);
    REQUIRE(c->id == 3);
    REQUIRE(c->price == 102);
    REQUIRE(c->quantity == 9);
    REQUIRE(pool.available() == 0);
    REQUIRE(pool.in_use() == 2);
}

TEST_CASE("order pool rejects invalid and duplicate releases", "[order_pool]") {
    OrderPool<1> pool;
    qsl::engine::Order external{99, Side::Buy, 100, 1};

    REQUIRE_FALSE(pool.release(nullptr));
    REQUIRE_FALSE(pool.release(&external));

    auto *a = pool.try_acquire(1, Side::Buy, 100, 5);
    REQUIRE(a != nullptr);
    REQUIRE(pool.owns(a));
    REQUIRE_FALSE(pool.owns(&external));

    REQUIRE(pool.release(a));
    REQUIRE_FALSE(pool.release(a));
    REQUIRE(pool.available() == 1);
    REQUIRE(pool.in_use() == 0);
}

TEST_CASE("order pool rejects interior slot pointers", "[order_pool]") {
    OrderPool<1> pool;
    auto *a = pool.try_acquire(1, Side::Buy, 100, 5);
    REQUIRE(a != nullptr);

    auto *interior = reinterpret_cast<qsl::engine::Order *>(reinterpret_cast<std::byte *>(a) + 1);
    REQUIRE_FALSE(pool.release(interior));

    REQUIRE(pool.release(a));
    REQUIRE(pool.available() == 1);
    REQUIRE(pool.in_use() == 0);
}

TEST_CASE("order pool reset frees all slots deterministically", "[order_pool]") {
    OrderPool<3> pool;
    auto *a = pool.try_acquire(1, Side::Buy, 100, 5);
    auto *b = pool.try_acquire(2, Side::Buy, 101, 5);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(pool.available() == 1);

    pool.reset();
    REQUIRE(pool.available() == 3);
    REQUIRE(pool.in_use() == 0);
    REQUIRE_FALSE(pool.release(a));
    REQUIRE_FALSE(pool.release(b));

    auto *c = pool.try_acquire(3, Side::Sell, 99, 2);
    REQUIRE(c != nullptr);
    REQUIRE(c->id == 3);
    REQUIRE(pool.available() == 2);
}

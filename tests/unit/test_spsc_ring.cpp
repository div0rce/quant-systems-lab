#include "qsl/concurrency/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>
#include <vector>

using qsl::concurrency::SpscRing;

TEST_CASE("empty ring: pop fails, empty/full report correctly", "[spsc]") {
    SpscRing<int, 4> ring;
    REQUIRE(ring.capacity() == 4);
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());
    int out = -1;
    REQUIRE_FALSE(ring.try_pop(out));
    REQUIRE(out == -1); // unchanged on empty pop
}

TEST_CASE("single push/pop round trip", "[spsc]") {
    SpscRing<int, 4> ring;
    REQUIRE(ring.try_push(42));
    REQUIRE_FALSE(ring.empty());
    int out = 0;
    REQUIRE(ring.try_pop(out));
    REQUIRE(out == 42);
    REQUIRE(ring.empty());
}

TEST_CASE("FIFO order is preserved", "[spsc]") {
    SpscRing<int, 8> ring;
    for (int i = 1; i <= 5; ++i) {
        REQUIRE(ring.try_push(i));
    }
    for (int i = 1; i <= 5; ++i) {
        int out = 0;
        REQUIRE(ring.try_pop(out));
        REQUIRE(out == i);
    }
}

TEST_CASE("full queue rejects and respects the capacity boundary", "[spsc]") {
    SpscRing<int, 3> ring;
    REQUIRE(ring.try_push(1));
    REQUIRE(ring.try_push(2));
    REQUIRE(ring.try_push(3)); // exactly capacity() items fit
    REQUIRE(ring.full());
    REQUIRE_FALSE(ring.try_push(4)); // full: rejected, queue unchanged
    int out = 0;
    REQUIRE(ring.try_pop(out));
    REQUIRE(out == 1);
    REQUIRE_FALSE(ring.full());
    REQUIRE(ring.try_push(4)); // a freed slot can be reused
}

TEST_CASE("indices wrap around correctly over many cycles", "[spsc]") {
    SpscRing<int, 4> ring;
    int expected = 0;
    int next_to_push = 0;
    // Push/pop far more than kSlots so head_/tail_ wrap multiple times.
    for (int cycle = 0; cycle < 1000; ++cycle) {
        while (ring.try_push(next_to_push)) {
            ++next_to_push;
        }
        int out = 0;
        while (ring.try_pop(out)) {
            REQUIRE(out == expected);
            ++expected;
        }
    }
    REQUIRE(expected == next_to_push);
}

TEST_CASE("single-producer/single-consumer stress preserves order and count", "[spsc]") {
    SpscRing<std::uint64_t, 1024> ring;
    constexpr std::uint64_t kCount = 200000;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield(); // backpressure: spin until a slot frees
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t next_expected = 0;
    bool in_order = true;
    std::thread consumer([&] {
        std::uint64_t value = 0;
        while (received < kCount) {
            if (ring.try_pop(value)) {
                if (value != next_expected) {
                    in_order = false;
                }
                ++next_expected;
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(in_order);
    REQUIRE(received == kCount);
    REQUIRE(ring.empty());
}

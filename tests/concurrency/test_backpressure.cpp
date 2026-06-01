// Backpressure and shutdown contract for the SPSC ring (docs/concurrency_model.md → Backpressure,
// Shutdown). The queue is bounded and never blocks: try_push reports false when full and try_pop
// reports false when empty, and the *caller* owns the policy. These tests pin the three documented
// behaviors: the spin/yield (lossless) policy, the drop-on-full policy, and drain-then-stop
// shutdown that loses nothing.

#include "qsl/concurrency/spsc_ring.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

using qsl::concurrency::SpscRing;

TEST_CASE("backpressure contract: full try_push returns false and leaves the queue unchanged",
          "[spsc][backpressure]") {
    SpscRing<int, 2> ring;
    REQUIRE(ring.try_push(10));
    REQUIRE(ring.try_push(20));
    REQUIRE(ring.full());

    // Full: the push is rejected and nothing is mutated; a later pop still sees the original front.
    REQUIRE_FALSE(ring.try_push(30));
    REQUIRE(ring.full());

    int out = 0;
    REQUIRE(ring.try_pop(out));
    REQUIRE(out == 10); // unchanged front: the rejected push did not overwrite or reorder anything
    REQUIRE_FALSE(ring.full());

    // Exactly one slot was freed, so exactly one push now succeeds and the next is rejected again.
    REQUIRE(ring.try_push(30));
    REQUIRE(ring.full());
    REQUIRE_FALSE(ring.try_push(40));
}

TEST_CASE("spin/yield policy: a stalled consumer applies real backpressure but loses nothing",
          "[spsc][backpressure]") {
    constexpr std::uint64_t kCount = 50000;
    SpscRing<std::uint64_t, 4> ring;

    std::atomic<bool> consumer_may_start{false};
    std::atomic<std::uint64_t> full_rejections{0};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) {
                // The queue is full: this is backpressure. Record it and release the consumer so it
                // can drain. Because the consumer waits for this flag, the producer is guaranteed
                // to observe at least one full queue (after the first `capacity()` items).
                full_rejections.fetch_add(1, std::memory_order_relaxed);
                consumer_may_start.store(true, std::memory_order_release);
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t next_expected = 0;
    bool in_order = true;
    std::thread consumer([&] {
        while (!consumer_may_start.load(std::memory_order_acquire)) {
            std::this_thread::yield(); // hold off until the producer has hit a full queue
        }
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

    REQUIRE(full_rejections.load() >= 1); // backpressure actually occurred
    REQUIRE(in_order);
    REQUIRE(received == kCount); // lossless: spinning on full delivered every item
    REQUIRE(ring.empty());
}

TEST_CASE("drop-on-full policy: dropped + received accounts for every produced item",
          "[spsc][backpressure]") {
    constexpr std::uint64_t kCount = 200000;
    SpscRing<std::uint64_t, 8> ring;

    std::atomic<bool> producer_done{false};
    std::uint64_t pushed = 0;
    std::uint64_t dropped = 0;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            if (ring.try_push(i)) {
                ++pushed;
            } else {
                ++dropped; // caller's policy: drop instead of spinning
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t received = 0;
    bool strictly_increasing = true;
    bool have_prev = false;
    std::uint64_t prev = 0;
    std::thread consumer([&] {
        std::uint64_t value = 0;
        for (;;) {
            if (ring.try_pop(value)) {
                if (have_prev && value <= prev) {
                    strictly_increasing = false; // FIFO over the surviving subsequence
                }
                prev = value;
                have_prev = true;
                ++received;
            } else if (producer_done.load(std::memory_order_acquire)) {
                // Producer finished; drain whatever it published before signalling done. The
                // acquire above pairs with the producer's release, so every pushed item is visible.
                while (ring.try_pop(value)) {
                    if (have_prev && value <= prev) {
                        strictly_increasing = false;
                    }
                    prev = value;
                    have_prev = true;
                    ++received;
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(strictly_increasing);
    REQUIRE(pushed + dropped == kCount); // every produced item was either enqueued or dropped
    REQUIRE(received == pushed);         // and every enqueued item was eventually received
    REQUIRE(received + dropped == kCount);
    REQUIRE(ring.empty());
}

TEST_CASE("drain-then-stop shutdown: an out-of-band stop flag loses no accepted item",
          "[spsc][backpressure][shutdown]") {
    constexpr std::uint64_t kCount = 100000;
    SpscRing<std::uint64_t, 64> ring;

    std::atomic<bool> stop{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
        stop.store(true, std::memory_order_release); // signal shutdown after the last publish
    });

    std::uint64_t received = 0;
    std::uint64_t next_expected = 0;
    bool in_order = true;
    std::thread consumer([&] {
        std::uint64_t value = 0;
        for (;;) {
            if (ring.try_pop(value)) {
                if (value != next_expected) {
                    in_order = false;
                }
                ++next_expected;
                ++received;
                continue;
            }
            if (stop.load(std::memory_order_acquire)) {
                // Drain-then-stop: everything published before `stop` happens-before this point, so
                // this final drain cannot miss a tail element.
                while (ring.try_pop(value)) {
                    if (value != next_expected) {
                        in_order = false;
                    }
                    ++next_expected;
                    ++received;
                }
                break;
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(in_order);
    REQUIRE(received == kCount); // no accepted item lost at shutdown
    REQUIRE(ring.empty());
}

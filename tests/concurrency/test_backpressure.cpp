// Backpressure and shutdown contract for the SPSC ring (docs/concurrency_model.md → Backpressure,
// Shutdown). The queue is bounded and never blocks: try_push reports false when full and try_pop
// reports false when empty, and the *caller* owns the policy. These tests pin the three documented
// behaviors: the spin/yield (lossless) policy, the drop-on-full policy, and drain-then-stop
// shutdown that loses nothing.

#include "qsl/concurrency/spsc_ring.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>

using qsl::concurrency::SpscRing;

template <class T, std::size_t Capacity> void require_empty(const SpscRing<T, Capacity> &ring) {
    REQUIRE(ring.empty());
}

struct OrderedReceiveState {
    std::uint64_t received = 0;
    std::uint64_t next_expected = 0;
    bool in_order = true;
};

struct SurvivorState {
    std::uint64_t received = 0;
    bool strictly_increasing = true;
    bool have_prev = false;
    std::uint64_t prev = 0;
};

struct DropCounts {
    std::uint64_t pushed = 0;
    std::uint64_t dropped = 0;
};

void fill_two_item_ring(SpscRing<int, 2> &ring) {
    REQUIRE(ring.try_push(10));
    REQUIRE(ring.try_push(20));
    REQUIRE(ring.full());
}

void require_push_rejected(SpscRing<int, 2> &ring, int value) {
    REQUIRE_FALSE(ring.try_push(value));
    REQUIRE(ring.full());
}

void require_next_pop(SpscRing<int, 2> &ring, int expected) {
    int out = 0;
    REQUIRE(ring.try_pop(out));
    REQUIRE(out == expected);
}

void require_lossless_spin_result(const std::atomic<std::uint64_t> &full_rejections,
                                  const OrderedReceiveState &state, std::uint64_t total) {
    REQUIRE(full_rejections.load() >= 1);
    REQUIRE(state.in_order);
    REQUIRE(state.received == total);
}

void require_drop_producer_accounting(const DropCounts &counts, std::uint64_t total) {
    REQUIRE(counts.pushed + counts.dropped == total);
}

void require_drop_consumer_accounting(const SurvivorState &state, const DropCounts &counts,
                                      std::uint64_t total) {
    REQUIRE(state.received == counts.pushed);
    REQUIRE(state.received + counts.dropped == total);
}

void record_expected(std::uint64_t value, OrderedReceiveState &state) {
    if (value != state.next_expected) {
        state.in_order = false;
    }
    ++state.next_expected;
    ++state.received;
}

void record_survivor(std::uint64_t value, SurvivorState &state) {
    if (state.have_prev && value <= state.prev) {
        state.strictly_increasing = false; // FIFO over the surviving subsequence
    }
    state.prev = value;
    state.have_prev = true;
    ++state.received;
}

void produce_lossless_after_backpressure(SpscRing<std::uint64_t, 4> &ring,
                                         std::atomic<bool> &consumer_may_start,
                                         std::atomic<std::uint64_t> &full_rejections,
                                         std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        while (!ring.try_push(i)) {
            // The queue is full: this is backpressure. Record it and release the consumer so it
            // can drain. Because the consumer waits for this flag, the producer is guaranteed to
            // observe at least one full queue (after the first `capacity()` items).
            full_rejections.fetch_add(1, std::memory_order_relaxed);
            consumer_may_start.store(true, std::memory_order_release);
            std::this_thread::yield();
        }
    }
}

void consume_after_backpressure(SpscRing<std::uint64_t, 4> &ring,
                                const std::atomic<bool> &consumer_may_start, std::uint64_t count,
                                OrderedReceiveState &state) {
    while (!consumer_may_start.load(std::memory_order_acquire)) {
        std::this_thread::yield(); // hold off until the producer has hit a full queue
    }

    std::uint64_t value = 0;
    while (state.received < count) {
        if (ring.try_pop(value)) {
            record_expected(value, state);
        } else {
            std::this_thread::yield();
        }
    }
}

void produce_with_drop_policy(SpscRing<std::uint64_t, 8> &ring, std::uint64_t count,
                              DropCounts &counts, std::atomic<bool> &producer_done) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (ring.try_push(i)) {
            ++counts.pushed;
        } else {
            ++counts.dropped; // caller's policy: drop instead of spinning
        }
    }
    producer_done.store(true, std::memory_order_release);
}

void drain_survivors(SpscRing<std::uint64_t, 8> &ring, SurvivorState &state) {
    std::uint64_t value = 0;
    while (ring.try_pop(value)) {
        record_survivor(value, state);
    }
}

void consume_drop_policy(SpscRing<std::uint64_t, 8> &ring, const std::atomic<bool> &producer_done,
                         SurvivorState &state) {
    std::uint64_t value = 0;
    for (;;) {
        if (ring.try_pop(value)) {
            record_survivor(value, state);
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            drain_survivors(ring, state);
            break;
        }
        std::this_thread::yield();
    }
}

void produce_until_stop(SpscRing<std::uint64_t, 64> &ring, std::atomic<bool> &stop,
                        std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        while (!ring.try_push(i)) {
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release); // signal shutdown after the last publish
}

void drain_expected(SpscRing<std::uint64_t, 64> &ring, OrderedReceiveState &state) {
    std::uint64_t value = 0;
    while (ring.try_pop(value)) {
        record_expected(value, state);
    }
}

void consume_until_stop(SpscRing<std::uint64_t, 64> &ring, const std::atomic<bool> &stop,
                        OrderedReceiveState &state) {
    std::uint64_t value = 0;
    for (;;) {
        if (ring.try_pop(value)) {
            record_expected(value, state);
            continue;
        }
        if (stop.load(std::memory_order_acquire)) {
            drain_expected(ring, state);
            break;
        }
        std::this_thread::yield();
    }
}

TEST_CASE("backpressure contract: full try_push returns false and leaves the queue unchanged",
          "[spsc][backpressure]") {
    SpscRing<int, 2> ring;
    fill_two_item_ring(ring);

    // Full: the push is rejected and nothing is mutated; a later pop still sees the original front.
    require_push_rejected(ring, 30);

    require_next_pop(ring, 10); // rejected push did not overwrite or reorder anything
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
        produce_lossless_after_backpressure(ring, consumer_may_start, full_rejections, kCount);
    });

    OrderedReceiveState receive;
    std::thread consumer(
        [&] { consume_after_backpressure(ring, consumer_may_start, kCount, receive); });

    producer.join();
    consumer.join();

    require_lossless_spin_result(full_rejections, receive, kCount);
    require_empty(ring);
}

TEST_CASE("drop-on-full policy: dropped + received accounts for every produced item",
          "[spsc][backpressure]") {
    constexpr std::uint64_t kCount = 200000;
    SpscRing<std::uint64_t, 8> ring;

    std::atomic<bool> producer_done{false};
    DropCounts counts;

    std::thread producer([&] { produce_with_drop_policy(ring, kCount, counts, producer_done); });

    SurvivorState survivors;
    std::thread consumer([&] { consume_drop_policy(ring, producer_done, survivors); });

    producer.join();
    consumer.join();

    REQUIRE(survivors.strictly_increasing);
    require_drop_producer_accounting(counts, kCount);
    require_drop_consumer_accounting(survivors, counts, kCount);
    require_empty(ring);
}

TEST_CASE("drain-then-stop shutdown: an out-of-band stop flag loses no accepted item",
          "[spsc][backpressure][shutdown]") {
    constexpr std::uint64_t kCount = 100000;
    SpscRing<std::uint64_t, 64> ring;

    std::atomic<bool> stop{false};

    std::thread producer([&] { produce_until_stop(ring, stop, kCount); });

    OrderedReceiveState receive;
    std::thread consumer([&] { consume_until_stop(ring, stop, receive); });

    producer.join();
    consumer.join();

    REQUIRE(receive.in_order);
    REQUIRE(receive.received == kCount); // no accepted item lost at shutdown
    require_empty(ring);
}

// Concurrency evidence for the SPSC ring (docs/memory_ordering.md, docs/concurrency_model.md).
//
// These are the empirical companions to the static memory-ordering argument: a sustained
// single-producer/single-consumer flow must preserve strict FIFO order, lose nothing, and end
// empty. A missing release/acquire pair would surface here as a dropped, duplicated, or reordered
// value (especially on weakly-ordered hardware) and, under `make asan`, as a sanitizer report.
// They do not replace ThreadSanitizer (M27); ASan/UBSan do not detect data races.

#include "qsl/concurrency/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>

using qsl::concurrency::SpscRing;

namespace {

// Push 0..count-1 through a ring, spinning on backpressure, and assert the consumer observes every
// value exactly once, in order, with the ring empty at the end.
template <class T, std::size_t Capacity> void run_inorder_flow(std::uint64_t count) {
    SpscRing<T, Capacity> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!ring.try_push(static_cast<T>(i))) {
                std::this_thread::yield(); // lossless backpressure: spin until a slot frees
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t next_expected = 0;
    bool in_order = true;
    std::thread consumer([&] {
        T value{};
        while (received < count) {
            if (ring.try_pop(value)) {
                if (static_cast<std::uint64_t>(value) != next_expected) {
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
    REQUIRE(received == count);
    REQUIRE(ring.empty());
}

} // namespace

TEST_CASE("SPSC stress: tiny capacity forces heavy backpressure but preserves FIFO",
          "[spsc][stress]") {
    // Capacity 2 means the queue is full almost continuously, so the acquire/release hand-off is
    // exercised on nearly every operation.
    run_inorder_flow<std::uint64_t, 2>(250000);
}

TEST_CASE("SPSC stress: larger ring sustains a long ordered flow", "[spsc][stress]") {
    run_inorder_flow<std::uint64_t, 1024>(500000);
}

TEST_CASE("SPSC stress: 32-bit payload wraps indices many times without loss", "[spsc][stress]") {
    // Capacity 7 (kSlots = 8) wraps the index modulus repeatedly over a long run.
    run_inorder_flow<std::uint32_t, 7>(300000);
}

TEST_CASE("SPSC stress: repeated short bursts leave the ring consistent", "[spsc][stress]") {
    // Construct and tear down many independent rings to catch state that leaks across a run's
    // boundary (it must not: each ring is a fresh, single-shot SPSC channel).
    for (int burst = 0; burst < 50; ++burst) {
        run_inorder_flow<std::uint64_t, 16>(4000);
    }
}

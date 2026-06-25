#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace qsl::concurrency {

// Bounded, lock-free single-producer/single-consumer (SPSC) ring buffer.
//
// Exactly one thread may call the producer side (`try_push`) and exactly one *different* thread
// may call the consumer side (`try_pop`). Concurrent producers, or concurrent consumers, are
// undefined behavior, this is intentionally **not** an MPMC queue (see
// docs/concurrency_model.md for why SPSC is sufficient here). Capacity is fixed at compile time
// and storage is inline, so `try_push`/`try_pop` never allocate.
//
// Synchronization is one acquire/release pair per direction:
//   - producer reads the consumer's index with `acquire`, publishes its own with `release`;
//   - consumer reads the producer's index with `acquire`, publishes its own with `release`.
// `head_` and `tail_` sit on separate cache lines to avoid false sharing.
template <class T, std::size_t Capacity> class SpscRing {
    static_assert(Capacity >= 1, "SpscRing capacity must be >= 1");

  public:
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer side. Enqueues a copy/move of `value`; returns false (queue unchanged) if full.
    [[nodiscard]] bool try_push(const T &value) { return emplace(value); }
    [[nodiscard]] bool try_push(T &&value) { return emplace(std::move(value)); }

    // Consumer side. Moves the front element into `out` and returns true; false if empty.
    [[nodiscard]] bool try_pop(T &out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false; // empty: nothing published past our read cursor
        }
        out = std::move(buffer_[head]);
        head_.store(next(head), std::memory_order_release);
        return true;
    }

    // Observational. `empty()` is authoritative only for the consumer thread, `full()` only for
    // the producer thread; either may be momentarily stale when observed from the other side.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool full() const noexcept {
        return next(tail_.load(std::memory_order_acquire)) == head_.load(std::memory_order_acquire);
    }

  private:
    static constexpr std::size_t kSlots = Capacity + 1; // one spare slot distinguishes full/empty
    static constexpr std::size_t kCacheLine = 64;

    static constexpr std::size_t next(std::size_t i) noexcept { return (i + 1) % kSlots; }

    template <class U> bool emplace(U &&value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t n = next(tail);
        if (n == head_.load(std::memory_order_acquire)) {
            return false; // full: advancing would collide with the consumer's read cursor
        }
        buffer_[tail] = std::forward<U>(value);
        tail_.store(n, std::memory_order_release);
        return true;
    }

    std::array<T, kSlots> buffer_{};
    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // next slot to read (consumer owns)
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // next slot to write (producer owns)
};

} // namespace qsl::concurrency

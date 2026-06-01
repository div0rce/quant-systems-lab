#pragma once

#include "qsl/engine/order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qsl::memory {

// Fixed-capacity pool for hot-path resting-order-like objects.
//
// The pool never grows and never falls back to heap allocation. Exhaustion is explicit:
// try_acquire returns nullptr when no slot is available.
template <std::size_t Capacity> class OrderPool {
    static_assert(Capacity >= 1, "OrderPool capacity must be >= 1");

  public:
    OrderPool() noexcept { reset_free_list(); }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return Capacity - free_count_; }

    [[nodiscard]] engine::Order *try_acquire(engine::OrderId id, engine::Side side,
                                             engine::Price price,
                                             engine::Quantity quantity) noexcept {
        if (free_count_ == 0) {
            return nullptr;
        }
        const std::size_t idx = free_indices_[--free_count_];
        storage_[idx] = engine::Order{id, side, price, quantity};
        live_[idx] = true;
        return &storage_[idx];
    }

    bool release(engine::Order *order) noexcept {
        std::size_t slot = 0;
        if (!slot_index(order, slot) || !live_[slot]) {
            return false;
        }
        live_[slot] = false;
        free_indices_[free_count_++] = slot;
        return true;
    }

    [[nodiscard]] bool owns(const engine::Order *order) const noexcept {
        std::size_t slot = 0;
        return slot_index(order, slot);
    }

    void reset() noexcept {
        live_.fill(false);
        reset_free_list();
    }

  private:
    void reset_free_list() noexcept {
        free_count_ = Capacity;
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_indices_[i] = Capacity - 1 - i;
        }
    }

    [[nodiscard]] bool slot_index(const engine::Order *order, std::size_t &slot) const noexcept {
        if (order == nullptr) {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
        const auto raw = reinterpret_cast<std::uintptr_t>(order);
        constexpr std::size_t width = sizeof(engine::Order);
        if (raw < base || raw >= base + (Capacity * width)) {
            return false;
        }
        const std::uintptr_t offset = raw - base;
        if (offset % width != 0) {
            return false;
        }
        slot = static_cast<std::size_t>(offset / width);
        return slot < Capacity;
    }

    std::array<engine::Order, Capacity> storage_{};
    std::array<bool, Capacity> live_{};
    std::array<std::size_t, Capacity> free_indices_{};
    std::size_t free_count_{0};
};

} // namespace qsl::memory

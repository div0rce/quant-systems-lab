#pragma once

#include "qsl/core/types.hpp"

#include <cstdint>

namespace qsl::feed {

using core::SeqNo;

/// Detects forward gaps in a market-data sequence (e.g. UDP datagram loss). It does not
/// recover lost messages, it only reports how many were missed. Duplicates and reordered
/// (lower) sequence numbers are ignored rather than reported as gaps.
class SequenceTracker {
  public:
    /// Observe a message's sequence number; returns how many messages were missed
    /// immediately before it (0 if contiguous or first observed).
    std::uint64_t observe(SeqNo seq) noexcept {
        if (!started_) {
            started_ = true;
            last_ = seq;
            return 0;
        }
        if (seq <= last_) {
            return 0; // duplicate or out-of-order; not a forward gap
        }
        const std::uint64_t missed = seq - last_ - 1;
        last_ = seq;
        return missed;
    }

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] SeqNo last() const noexcept { return last_; }

  private:
    bool started_ = false;
    SeqNo last_ = 0;
};

} // namespace qsl::feed

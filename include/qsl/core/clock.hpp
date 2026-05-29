#pragma once

#include "qsl/core/types.hpp"

namespace qsl::core {

/// Deterministic, monotonic logical clock. Independent of wall-clock time so
/// engine paths remain replayable. `tick` advances and returns the new value.
class LogicalClock {
  public:
    [[nodiscard]] Timestamp now() const noexcept { return time_; }

    Timestamp tick() noexcept { return ++time_; }

    void advance(Timestamp by) noexcept { time_ += by; }

  private:
    Timestamp time_{0};
};

} // namespace qsl::core

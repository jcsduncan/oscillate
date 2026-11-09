#pragma once

#include "oscillate/project/snapshot.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

namespace oscillate::quick {

// The furthest zero-based bar the arrangement addresses.
inline constexpr double max_arrangement_bar = 1'000'000.0;

// Zero-based fractional bars, as the arrangement draws them, and project
// ticks, through the project's meter map.
[[nodiscard]] inline std::optional<std::int64_t> tick_of_bar(const project::Snapshot& snapshot,
                                                             double bar) {
  if (!std::isfinite(bar) || bar < 0.0 || bar > max_arrangement_bar)
    return std::nullopt;
  const auto whole = static_cast<std::int64_t>(std::floor(bar));
  const auto start = snapshot.meter.tick_at({whole + 1, 1, 0});
  const auto next = snapshot.meter.tick_at({whole + 2, 1, 0});
  if (!start || !next)
    return std::nullopt;
  return start.value().value +
         std::llround((bar - static_cast<double>(whole)) *
                      static_cast<double>(next.value().value - start.value().value));
}

[[nodiscard]] inline std::optional<double> bar_of_tick(const project::Snapshot& snapshot,
                                                       std::int64_t tick) {
  const auto located = snapshot.meter.locate({tick});
  if (!located)
    return std::nullopt;
  const auto start = snapshot.meter.tick_at({located.value().bar, 1, 0});
  const auto next = snapshot.meter.tick_at({located.value().bar + 1, 1, 0});
  if (!start || !next || next.value().value <= start.value().value)
    return std::nullopt;
  return static_cast<double>(located.value().bar - 1) +
         static_cast<double>(tick - start.value().value) /
             static_cast<double>(next.value().value - start.value().value);
}

} // namespace oscillate::quick

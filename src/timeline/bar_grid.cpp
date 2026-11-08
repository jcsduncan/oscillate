#include "oscillate/timeline/bar_grid.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace oscillate::timeline {

BarGrid::BarGrid(const MeterMap& meter) {
  const auto& events = meter.events();
  segments_.reserve(events.size());
  double bar = 0.0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& event = events[i];
    const double ticks_per_bar = static_cast<double>(ticks_per_quarter) * 4.0 * event.numerator /
                                 static_cast<double>(event.denominator);
    if (i > 0) {
      const auto& previous = segments_.back();
      bar += static_cast<double>(event.tick.value - previous.tick) / previous.ticks_per_bar;
    }
    segments_.push_back({event.tick.value, ticks_per_bar, bar});
  }
}

void BarGrid::locate(std::int64_t tick, double& bar, double& beats_per_bar) const noexcept {
  constexpr double quarter = static_cast<double>(ticks_per_quarter);
  if (segments_.empty() || tick < segments_.front().tick) {
    beats_per_bar = 4.0;
    bar = static_cast<double>(tick) / (4.0 * quarter);
    return;
  }
  const auto after = std::ranges::upper_bound(segments_, tick, {}, &Segment::tick);
  const auto& segment = *(after - 1);
  beats_per_bar = segment.ticks_per_bar / quarter;
  bar = segment.first_bar + static_cast<double>(tick - segment.tick) / segment.ticks_per_bar;
}

} // namespace oscillate::timeline

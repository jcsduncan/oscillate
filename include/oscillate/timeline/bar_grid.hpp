#pragma once

#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <vector>

namespace oscillate::timeline {

// The meter as bar lines, for the audio callback: a copy of the meter map's
// segments that answers "which bar, and how long is it" by binary search,
// with no allocation and no error path.
class BarGrid {
public:
  BarGrid() = default;
  explicit BarGrid(const MeterMap& meter);
  // Continuous, zero-based bar position and quarter-note beats per bar at a
  // tick. Before the first segment, 4/4 from tick zero.
  void locate(std::int64_t tick, double& bar, double& beats_per_bar) const noexcept;

private:
  struct Segment {
    std::int64_t tick = 0;
    double ticks_per_bar = 0.0;
    double first_bar = 0.0;
  };
  std::vector<Segment> segments_;
};

} // namespace oscillate::timeline

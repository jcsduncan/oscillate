#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::persistence {

// A groove template (ADR-0059): the feel of one cycle of grid steps, kept
// as a user file under $XDG_DATA_HOME/oscillate/grooves/. Timing is a
// fraction of a step (+0.25 plays a quarter of a 16th late), so a groove
// is tempo-free; velocity scales a note's velocity (1 leaves it).
struct GrooveStep {
  double timing = 0.0;
  double velocity = 1.0;
  bool operator==(const GrooveStep&) const = default;
};
inline constexpr double max_groove_timing = 0.5;
inline constexpr double max_groove_velocity = 2.0;
inline constexpr std::size_t max_groove_steps = 64;
inline constexpr std::size_t max_groove_name_bytes = 128;
inline constexpr std::size_t max_groove_file_bytes = 64U * 1024U;

struct GrooveTemplate {
  std::string name;
  // Steps per quarter note: 2 is an 8th grid, 4 a 16th grid.
  std::uint32_t steps_per_quarter = 4;
  // One cycle; the groove repeats every steps.size() steps from tick 0.
  std::vector<GrooveStep> steps;
  bool operator==(const GrooveTemplate&) const = default;
};

// Structure and ranges: a name, 1/2/3/4/6/8 steps per quarter, 1 to
// max_groove_steps steps, timings within +/-max_groove_timing and
// velocities within [0, max_groove_velocity].
[[nodiscard]] foundation::Result<void> validate_groove(const GrooveTemplate& groove);
[[nodiscard]] std::string encode_groove(const GrooveTemplate& groove);
[[nodiscard]] foundation::Result<GrooveTemplate> decode_groove(std::string_view text);

} // namespace oscillate::persistence

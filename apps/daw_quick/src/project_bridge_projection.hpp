#pragma once

#include "automation_projection.hpp"

#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <optional>

namespace oscillate::quick::detail {

// A zero-based, fractional bar position as a tick: whole bars by the meter,
// then the fraction across that bar. The arrangement snaps to its grid
// before it calls, so this keeps whatever position it chose.
[[nodiscard]] std::optional<timeline::TickPosition> tick_at_bars(const project::Snapshot& snapshot,
                                                                 double bars);

} // namespace oscillate::quick::detail

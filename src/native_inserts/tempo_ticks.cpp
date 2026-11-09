#include "oscillate/native_inserts/tempo_ticks.hpp"

#include <cstdint>
#include <limits>

namespace oscillate::native_inserts {

bool TempoTicks::tick_of(std::uint64_t frame, std::int64_t& tick) const noexcept {
  if (sample_rate_ == 0 ||
      frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  timeline::TickPosition position{};
  if (!tempo_->sample_to_tick_rt({static_cast<std::int64_t>(frame)}, sample_rate_, position))
    return false;
  tick = position.value;
  return true;
}

} // namespace oscillate::native_inserts

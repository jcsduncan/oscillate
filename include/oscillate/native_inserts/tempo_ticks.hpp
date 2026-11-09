#pragma once

#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>

namespace oscillate::native_inserts {

// The note effects' clock source: project frames to project ticks through
// the shared tempo map, at the project rate. Callback safe; holds the map by
// reference, so the map must outlive it.
class TempoTicks final : public native_dsp::note_effects::FrameTicks {
public:
  TempoTicks(const timeline::TempoMap& tempo, std::uint32_t sample_rate) noexcept
      : tempo_(&tempo), sample_rate_(sample_rate) {}
  // False for a zero rate, a frame past the signed sample range or one the
  // map cannot place.
  [[nodiscard]] bool tick_of(std::uint64_t frame, std::int64_t& tick) const noexcept override;

private:
  const timeline::TempoMap* tempo_;
  std::uint32_t sample_rate_;
};

} // namespace oscillate::native_inserts

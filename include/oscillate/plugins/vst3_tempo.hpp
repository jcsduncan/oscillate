#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// The musical time a VST3 graph node passes its plugin, as neutral inputs:
// the host compiles them from the project's tempo map (ADR-0104), so the
// plugins module evaluates no tempo map itself.
namespace oscillate::plugins {

namespace detail {

inline constexpr std::uint64_t maximum_live_sample = std::uint64_t{1} << 40U;

// A sample from which the tempo is a new bpm. Several events that round to
// one sample keep the last one's bpm.
struct TempoBoundary {
  std::uint64_t sample;
  double bpm;
};

} // namespace detail

// The project's musical clock at a project sample, read on the callback:
// no allocation, lock or unbounded work. False when the sample cannot be
// placed.
class Vst3TempoClock {
public:
  virtual ~Vst3TempoClock() = default;
  [[nodiscard]] virtual bool quarter_notes_at(std::int64_t sample,
                                              double& quarter_notes) const noexcept = 0;
  // The bpm at a sample inside a tempo ramp.
  [[nodiscard]] virtual bool bpm_at(std::int64_t sample, double& bpm) const noexcept = 0;
};

// Prepared on control. Blocks are split at the boundaries; the clock gives
// each segment's position, and its bpm when the map has ramps.
struct Vst3Tempo {
  std::vector<detail::TempoBoundary> boundaries;
  bool ramps = false;
  std::shared_ptr<const Vst3TempoClock> clock;
};

} // namespace oscillate::plugins

#pragma once

#include "oscillate/engine/graph.hpp"

#include <optional>
#include <span>

namespace oscillate::native_dsp {

// Stereo phase correlation for the master's mono check. +1 is mono (both
// channels the same), 0 is unrelated or a quarter cycle apart, -1 is out of
// phase: it cancels when summed to mono. Each product is smoothed by a
// one-pole of the given time constant (300 ms, a correlation meter's usual
// integration), so a reading is the recent past, not one block. One thread
// processes and reads; callers publish across threads themselves.
class StereoCorrelationMeter final {
public:
  void prepare(double sample_rate, double time_constant_seconds = 0.3) noexcept;
  void reset() noexcept;
  void process(std::span<const engine::StereoFrame> frames) noexcept;
  // Absent while both channels are below -100 dBFS: silence has no phase.
  [[nodiscard]] std::optional<double> correlation() const noexcept;
  // How much quieter the mix gets folded to mono, in dB (0 when both sides
  // are the same, about -3 when unrelated, very low when out of phase):
  // the mono sum's power against the mean channel power. Absent in silence.
  [[nodiscard]] std::optional<double> mono_loss_db() const noexcept;

private:
  double coefficient_ = 0.0;
  double left_power_ = 0.0;
  double right_power_ = 0.0;
  double cross_ = 0.0;
};

} // namespace oscillate::native_dsp

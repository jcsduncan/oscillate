#pragma once

#include "oscillate/native_dsp/synth/modulation.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::native_dsp::synth {

enum class ChorusMode : std::uint8_t { off, one, two, both };

// Stereo bucket-brigade style chorus after the Juno-60 modes: one triangle
// LFO sweeps the left delay and its inverse sweeps the right. Mode changes
// glide the delay centre, depth and wet level, so switching never clicks.
class Chorus {
public:
  struct Frame {
    double left;
    double right;
  };

  static constexpr double max_delay_seconds = 0.006;
  static constexpr double tail_seconds = max_delay_seconds;

  // Control only: allocates the delay lines for this rate.
  void prepare(double sample_rate);
  void reset() noexcept;
  // mix 0..1, where 1 is the classic equal dry/wet blend.
  void set(ChorusMode mode, double mix) noexcept;
  [[nodiscard]] Frame process(double left, double right) noexcept;

private:
  [[nodiscard]] double read(const std::vector<float>& line, double delay_frames) const noexcept;

  double rate_ = 48'000.0;
  std::vector<float> left_line_;
  std::vector<float> right_line_;
  std::size_t mask_ = 0;
  std::size_t write_ = 0;
  double lfo_phase_ = 0.0;
  double lfo_increment_ = 0.0;
  LinearRamp centre_;
  LinearRamp depth_;
  LinearRamp wet_;
  double tone_left_ = 0.0;
  double tone_right_ = 0.0;
  double tone_coefficient_ = 0.0;
  ChorusMode mode_ = ChorusMode::off;
  double mix_ = 0.0;
};

} // namespace oscillate::native_dsp::synth

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oscillate::native_dsp::synth {

// A short linear crossfade from an old voice configuration (filter type,
// algorithm, model, unison count) to the new one, so structural changes
// apply to sounding notes without a click (ADR-0038). Both configurations
// are rendered from the same input while it runs; linear because the two
// signals are strongly correlated.
class SwitchFade {
public:
  static constexpr double seconds = 0.02;

  void prepare(double rate) noexcept {
    frames_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(seconds * rate)));
  }
  void start() noexcept { remaining_ = frames_; }
  void stop() noexcept { remaining_ = 0; }
  [[nodiscard]] bool active() const noexcept { return remaining_ > 0; }
  // The old configuration's weight for the next frame, falling from just
  // under 1 to 0; 0 once the fade is over.
  [[nodiscard]] double next() noexcept {
    if (remaining_ == 0) {
      return 0.0;
    }
    --remaining_;
    return static_cast<double>(remaining_) / static_cast<double>(frames_);
  }

private:
  std::uint32_t frames_ = 960;
  std::uint32_t remaining_ = 0;
};

} // namespace oscillate::native_dsp::synth

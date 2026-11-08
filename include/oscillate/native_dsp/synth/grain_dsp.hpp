#pragma once

#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::native_dsp::synth {

// ---- Source buffer -----------------------------------------------------------

// A float ring indexed by absolute write count, read with 4-point cubic
// Hermite interpolation. Allocates in prepare only. Readers keep their
// positions inside the written, not-yet-overwritten range (ADR-0033).
//
// The source is also written, through a chain of half-band decimators, into
// mip levels at 1/2, 1/4, 1/8 and 1/16 of the rate, each holding the same
// span of time. A grain played at rate r reads the level where its rate is
// at most 1, so pitched-up grains never alias.
class GrainBuffer {
public:
  static constexpr std::size_t levels = 5;

  void prepare(std::size_t frames);
  void reset() noexcept;
  void write(double value) noexcept {
    put(0, value);
    ++count_;
  }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  // Usable history in frames.
  [[nodiscard]] std::size_t length() const noexcept { return length_; }
  // The level a grain at this playback rate reads (rate <= 2^level).
  [[nodiscard]] static std::size_t level_for(double rate) noexcept {
    std::size_t level = 0;
    while (level + 1 < levels && rate > static_cast<double>(std::size_t{1} << level)) {
      ++level;
    }
    return level;
  }
  // Full-rate read; position >= 1 and <= count - 3, in absolute frames.
  [[nodiscard]] double read(double position) const noexcept { return read(0, position); }
  // position in the level's own frames (full-rate frames / 2^level).
  [[nodiscard]] double read(std::size_t level, double position) const noexcept {
    const auto& data = data_[level];
    const std::size_t mask = masks_[level];
    const double floor = fast_floor(position);
    const auto index = static_cast<std::uint64_t>(floor);
    const double t = position - floor;
    const double y0 = data[(index - 1) & mask];
    const double y1 = data[index & mask];
    const double y2 = data[(index + 1) & mask];
    const double y3 = data[(index + 2) & mask];
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
  }

private:
  // Writes a sample into `level`, and every second one onwards, decimated.
  void put(std::size_t level, double value) noexcept {
    data_[level][counts_[level] & masks_[level]] = static_cast<float>(value);
    ++counts_[level];
    if (level + 1 == levels) {
      return;
    }
    if (!holding_[level]) {
      held_[level] = value;
      holding_[level] = true;
      return;
    }
    holding_[level] = false;
    put(level + 1, decimators_[level].process(held_[level], value));
  }

  std::array<std::vector<float>, levels> data_{};
  std::array<std::size_t, levels> masks_{};
  std::array<std::uint64_t, levels> counts_{};
  std::array<Downsampler2x, levels - 1> decimators_{};
  std::array<double, levels - 1> held_{};
  std::array<bool, levels - 1> holding_{};
  std::size_t length_ = 0;
  std::uint64_t count_ = 0;
};

// ---- Grain cloud ----------------------------------------------------------------

enum class GrainShape : std::uint8_t { hann, tukey, decay, swell };
enum class GrainIntervals : std::uint8_t { off, octaves, fifths, pentatonic };

struct GrainSettings {
  double size_frames = 9'600.0;     // Grain length at the output rate.
  double interval_frames = 960.0;   // Free spawning: mean spacing.
  double chaos = 0.0;               // 0..1 spacing irregularity.
  bool synced = false;              // Spawn on tempo divisions instead.
  double frames_per_division = 0.0; // Synced spacing.
  GrainShape shape = GrainShape::hann;
  double lag_frames = 4'800.0; // Scan centre behind the write head.
  double scan = 1.0;           // Centre speed; 1 keeps pace with the writer.
  double spray_frames = 0.0;   // Maximum start offset either way.
  double rate = 1.0;           // Base playback rate (pitch offset).
  GrainIntervals intervals = GrainIntervals::off;
  double interval_probability = 0.0;
  double jitter_cents = 0.0;
  double reverse_probability = 0.0;
  double spread = 0.0; // 0..1 stereo scatter.
  // Frozen only: the oldest frames still being overwritten by a writer that
  // runs on after the freeze to finish grains already in flight.
  double reserved_frames = 0.0;
};

// One voice's grains: an integer-frame clock from reset(), a fixed pool and
// a seeded generator, so every block split and every render of the same
// notes scatters identically.
class GrainCloud {
public:
  static constexpr std::size_t max_grains = 32;
  // Margin for a writer that fills a whole control chunk before grains read.
  static constexpr double writer_margin = 64.0;

  void reset(std::uint32_t seed) noexcept;
  // Control rate. A changed synced spacing rebases the clock at the current
  // frame, like the step sequencer.
  void set(const GrainSettings& settings) noexcept;
  // Adds `frames` frames of grains into left/right. The buffer holds the
  // source written so far; when live, the writer has already written this
  // chunk, so frame i sees count_before + i + 1 samples.
  void render(const GrainBuffer& buffer, bool frozen, std::uint64_t count_before, double* left,
              double* right, std::size_t frames) noexcept;

  [[nodiscard]] std::size_t active() const noexcept;
  // One sounding grain, as telemetry sees it: its position in full-rate
  // frames, its playback rate (> 0), its direction and its phase 0..1
  // through its window.
  struct GrainView {
    double position = 0.0;
    double rate = 0.0;
    bool reverse = false;
    double phase = 0.0;
  };
  // Visits each sounding grain: f(GrainView). For telemetry; touches nothing.
  template <typename F> void for_each_grain(F&& f) const {
    for (const auto& grain : grains_) {
      if (grain.active) {
        const double scale = static_cast<double>(std::size_t{1} << grain.level);
        f(GrainView{.position = grain.position * scale,
                    .rate = std::fabs(grain.step) * scale,
                    .reverse = grain.step < 0.0,
                    .phase = static_cast<double>(grain.age) / static_cast<double>(grain.length)});
      }
    }
  }
  [[nodiscard]] std::uint64_t elapsed() const noexcept { return elapsed_; }
  // Overlap-normalising gain for these settings: sqrt(2 / max(2, overlap)).
  [[nodiscard]] static double density_gain(const GrainSettings& settings) noexcept;
  [[nodiscard]] static double window(GrainShape shape, double x) noexcept;

private:
  struct Grain {
    bool active = false;
    std::size_t level = 0; // Mip level read; position and step in its frames.
    double position = 0.0;
    double step = 1.0;
    std::uint32_t age = 0;
    std::uint32_t length = 0;
    double left = 0.0;
    double right = 0.0;
    // Recursive cosine for the Hann and Tukey windows: exact to ~1e-12.
    double cos_now = 1.0;
    double cos_previous = 1.0;
    double cos_coefficient = 2.0;
    std::uint32_t taper = 0; // Tukey taper length in frames.
  };

  void spawn(const GrainBuffer& buffer, bool frozen, std::uint64_t written) noexcept;
  void schedule_next() noexcept;
  [[nodiscard]] double next_window(Grain& grain) const noexcept;
  [[nodiscard]] double uniform() noexcept { return random_.white(); } // [-1, 1)

  GrainSettings settings_{};
  double gain_ = 1.0;
  std::array<Grain, max_grains> grains_{};
  NoiseSource random_{};
  std::uint64_t elapsed_ = 0;
  std::uint64_t next_spawn_ = 0;
  std::int64_t spawned_ = 0;
  std::uint64_t base_frame_ = 0;
  double base_index_ = 0.0;
  double division_ = 0.0;
  double drift_ = 0.0; // Accumulated (writer speed - scan) frames.
};

// ---- Partial bank ----------------------------------------------------------------

// Sixteen sine partials. Partial n (1-based) runs at n f sqrt(1 + B n^2), the
// stiff-string law; amplitudes fall by n^-tilt with an odd/even balance.
// Partials at or above 0.45 of the rate are silent. Each partial is a
// complex rotation (four multiplies a frame, vectorisable), renormalised at
// every set() so its amplitude never drifts.
class PartialBank {
public:
  static constexpr std::size_t count = 16;
  void reset() noexcept;
  // Control rate. increment: fundamental cycles per frame.
  void set(double increment, double tilt, double odd, double stretch) noexcept;
  [[nodiscard]] double next() noexcept {
    double sum = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
      sum += amplitudes_[n] * sin_[n];
      const double c = cos_[n] * rotate_cos_[n] - sin_[n] * rotate_sin_[n];
      sin_[n] = sin_[n] * rotate_cos_[n] + cos_[n] * rotate_sin_[n];
      cos_[n] = c;
    }
    return sum;
  }
  [[nodiscard]] static double ratio(std::size_t n, double stretch) noexcept {
    const double k = static_cast<double>(n);
    return k * std::sqrt(1.0 + stretch * k * k);
  }

private:
  std::array<double, count> cos_{};
  std::array<double, count> sin_{};
  std::array<double, count> rotate_cos_{};
  std::array<double, count> rotate_sin_{};
  std::array<double, count> amplitudes_{};
  double increment_ = -1.0;
  double stretch_ = -1.0;
  double tilt_ = -1.0;
  double odd_ = -2.0;
};

// ---- Shimmer reverb ---------------------------------------------------------------

// Octave-up pitch shifter: two taps sweep a 50 ms window at twice speed,
// half a window apart, with complementary sin^2 crossfades (gain <= 1). The
// input is first band-limited below a quarter of the rate (an 8th-order
// Butterworth low-pass at 0.2 of the rate), so doubling never folds.
class OctaveShifter {
public:
  void prepare(double rate);
  void reset() noexcept;
  [[nodiscard]] double process(double x) noexcept;

private:
  std::array<ZdfSvf, 4> band_limit_{};
  std::array<SvfCoefficients, 4> band_coefficients_{};
  DelayLine line_;
  double window_ = 2'400.0;
  double phase_ = 0.0;
};

// Eight-line feedback delay network with a Householder mix, per-line decay
// gains for an exact RT60 at DC, one-pole damping, slow delay modulation and
// an octave-up shifter crossfaded into each line's feedback (ADR-0033).
class ShimmerReverb {
public:
  static constexpr std::size_t lines = 8;
  void prepare(double rate);
  void reset() noexcept;
  // size, damping, shimmer and mix 0..1; decay is the RT60 in seconds.
  void set(double size, double decay_seconds, double damping, double shimmer, double mix) noexcept;
  [[nodiscard]] StereoOut process(double left, double right) noexcept;
  // Time for the tail to fall 90 dB after the input stops.
  [[nodiscard]] static double tail_seconds(double decay_seconds) noexcept {
    return 1.5 * decay_seconds + 0.2;
  }

private:
  double rate_ = 48'000.0;
  std::array<DelayLine, lines> delays_{};
  std::array<OctaveShifter, lines> shifters_{};
  std::array<double, lines> length_{};
  std::array<double, lines> gain_{};
  std::array<double, lines> low_{};
  std::array<double, lines> lfo_phase_{};
  std::array<double, lines> lfo_increment_{};
  double depth_ = 14.0;
  double damping_ = 1.0; // One-pole coefficient; 1 is no damping.
  double shimmer_ = 0.0;
  double mix_ = 0.0;
};

} // namespace oscillate::native_dsp::synth

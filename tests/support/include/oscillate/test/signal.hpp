#pragma once

// Test signals and level measurements. Generators are deterministic: noise is
// seeded, sines start at the phase given. Level helpers take the samples as a
// span (pass a subspan for a window) and decibel conversions take an explicit
// floor, because tests disagree on what "silent" reads as.

#include "oscillate/foundation/split_mix.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::test {

inline constexpr double decibels_per_decade = 20.0;

// A floor far below any audible level: log10 of it stays finite.
inline constexpr double silence_floor = 1e-30;

// `frames` samples of amplitude * sin(2 pi f n / rate + phase).
template <typename Sample = double>
[[nodiscard]] std::vector<Sample> sine(double frequency, double rate, std::size_t frames,
                                       double amplitude = 1.0, double phase = 0.0) {
  std::vector<Sample> out(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    out[n] = static_cast<Sample>(
        amplitude *
        std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(n) / rate + phase));
  }
  return out;
}

// Uniform white noise in [-amplitude, amplitude) from SplitMix64.
template <typename Sample = double>
[[nodiscard]] std::vector<Sample> white_noise(std::size_t frames, std::uint64_t seed,
                                              double amplitude = 1.0) {
  foundation::SplitMix64 random(seed);
  std::vector<Sample> out(frames);
  for (auto& sample : out) {
    sample = static_cast<Sample>(amplitude * (2.0 * random.unit() - 1.0));
  }
  return out;
}

// Root mean square, accumulated in double in sample order.
template <typename Sample> [[nodiscard]] double rms(std::span<const Sample> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  double power = 0.0;
  for (const auto sample : samples) {
    const auto value = static_cast<double>(sample);
    power += value * value;
  }
  return std::sqrt(power / static_cast<double>(samples.size()));
}

template <typename Sample> [[nodiscard]] double rms(const std::vector<Sample>& samples) {
  return rms(std::span<const Sample>(samples));
}

// rms of samples [from, to).
template <typename Sample>
[[nodiscard]] double rms(const std::vector<Sample>& samples, std::size_t from, std::size_t to) {
  return rms(std::span<const Sample>(samples).subspan(from, to - from));
}

// Largest absolute sample.
template <typename Sample> [[nodiscard]] double peak(std::span<const Sample> samples) {
  double largest = 0.0;
  for (const auto sample : samples) {
    largest = std::max(largest, std::abs(static_cast<double>(sample)));
  }
  return largest;
}

template <typename Sample> [[nodiscard]] double peak(const std::vector<Sample>& samples) {
  return peak(std::span<const Sample>(samples));
}

// 20 log10(gain). Without a floor, zero reads -inf.
[[nodiscard]] inline double gain_to_db(double gain) {
  return decibels_per_decade * std::log10(gain);
}

// 20 log10(max(gain, floor)): silence reads as the floor's level.
[[nodiscard]] inline double gain_to_db(double gain, double floor) {
  return decibels_per_decade * std::log10(std::max(gain, floor));
}

[[nodiscard]] inline double db_to_gain(double db) {
  return std::pow(10.0, db / decibels_per_decade);
}

} // namespace oscillate::test

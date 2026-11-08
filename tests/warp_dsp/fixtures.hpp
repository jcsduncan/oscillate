#pragma once

// Deterministic synthetic fixtures with known ground truth for warp tests.
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace warp_fixtures {

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
  // Ground-truth attack starts (first sample of each event).
  std::vector<std::uint64_t> onsets;
};

class Random {
public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  double uniform() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11) / 9007199254740992.0;
  }
  double symmetric() { return 2.0 * uniform() - 1.0; }

private:
  std::uint64_t state_;
};

inline Stereo silence(std::size_t frames) {
  return {std::vector<float>(frames, 0.0F), std::vector<float>(frames, 0.0F), {}};
}

inline void add_noise_floor(Stereo& signal, double amplitude, std::uint64_t seed) {
  Random random(seed);
  for (std::size_t index = 0; index < signal.left.size(); ++index) {
    signal.left[index] += static_cast<float>(amplitude * random.symmetric());
    signal.right[index] += static_cast<float>(amplitude * random.symmetric());
  }
}

inline Stereo click_train(std::uint32_t rate, double seconds, double interval) {
  auto signal = silence(static_cast<std::size_t>(seconds * rate));
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double time = 0.25; time < seconds - 0.1; time += interval) {
    const auto at = static_cast<std::uint64_t>(std::llround(time * rate));
    signal.left[at] = 0.8F;
    signal.right[at] = 0.8F;
    signal.onsets.push_back(at);
  }
  return signal;
}

// Kick + snare-like hits: 1 ms linear attack, exponential decays.
inline Stereo drum_hits(std::uint32_t rate, double seconds, double interval, std::uint64_t seed) {
  auto signal = silence(static_cast<std::size_t>(seconds * rate));
  Random random(seed);
  int hit = 0;
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double time = 0.3; time < seconds - 0.4; time += interval, ++hit) {
    const auto at = static_cast<std::uint64_t>(std::llround(time * rate));
    signal.onsets.push_back(at);
    const bool kick = hit % 2 == 0;
    const double gain = 0.4 + 0.4 * random.uniform();
    const auto length = static_cast<std::size_t>(0.35 * rate);
    const double attack = 0.001 * rate;
    for (std::size_t offset = 0; offset < length && at + offset < signal.left.size(); ++offset) {
      const double t = static_cast<double>(offset) / rate;
      const double ramp = std::min(1.0, static_cast<double>(offset + 1) / attack);
      double value = 0.0;
      if (kick) {
        const double frequency = 55.0 + 90.0 * std::exp(-t / 0.03);
        value = std::sin(2.0 * std::numbers::pi * frequency * t) * std::exp(-t / 0.12) +
                0.3 * random.symmetric() * std::exp(-t / 0.005);
      } else {
        value = random.symmetric() * std::exp(-t / 0.06) +
                0.4 * std::sin(2.0 * std::numbers::pi * 190.0 * t) * std::exp(-t / 0.08);
      }
      const auto sample = static_cast<float>(gain * ramp * value);
      signal.left[at + offset] += sample;
      signal.right[at + offset] += sample * 0.9F;
    }
  }
  return signal;
}

// Overlapping harmonic plucks (legato): each note rings under the next.
inline Stereo plucks(std::uint32_t rate, double seconds, double interval, std::uint64_t seed) {
  auto signal = silence(static_cast<std::size_t>(seconds * rate));
  Random random(seed);
  constexpr double scale[] = {110.0, 146.83, 164.81, 220.0, 293.66, 329.63, 440.0, 587.33};
  std::size_t last_choice = 8;
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double time = 0.3; time < seconds - 0.4; time += interval) {
    const auto at = static_cast<std::uint64_t>(std::llround(time * rate));
    signal.onsets.push_back(at);
    // A melody never restrikes the same pitch back to back.
    auto choice = static_cast<std::size_t>(random.uniform() * 8.0) % 8;
    if (choice == last_choice)
      choice = (choice + 3) % 8;
    last_choice = choice;
    const double f0 = scale[choice];
    const double gain = 0.15 + 0.15 * random.uniform();
    const auto length = static_cast<std::size_t>(1.2 * rate);
    for (std::size_t offset = 0; offset < length && at + offset < signal.left.size(); ++offset) {
      const double t = static_cast<double>(offset) / rate;
      const double ramp = std::min(1.0, static_cast<double>(offset + 1) / (0.002 * rate));
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 6; ++harmonic)
        value += std::sin(2.0 * std::numbers::pi * f0 * harmonic * t) / harmonic *
                 std::exp(-t * (2.0 + harmonic));
      // Plectrum noise: a short broadband burst at the attack.
      value += 0.5 * random.symmetric() * std::exp(-t / 0.003);
      const auto sample = static_cast<float>(gain * ramp * value);
      signal.left[at + offset] += sample;
      signal.right[at + offset] += sample;
    }
  }
  return signal;
}

// Sustained chord with a slow fade-in and 5 Hz vibrato: no true onsets.
inline Stereo vibrato_pad(std::uint32_t rate, double seconds) {
  auto signal = silence(static_cast<std::size_t>(seconds * rate));
  constexpr double chord[] = {220.0, 277.18, 329.63};
  for (std::size_t index = 0; index < signal.left.size(); ++index) {
    const double t = static_cast<double>(index) / rate;
    const double fade = std::min(1.0, t / 1.5);
    double value = 0.0;
    for (const double f0 : chord) {
      // Phase of a tone with +-30 cent sinusoidal vibrato.
      const double depth = f0 * (std::pow(2.0, 30.0 / 1200.0) - 1.0);
      const double phase = 2.0 * std::numbers::pi *
                           (f0 * t - depth / (2.0 * std::numbers::pi * 5.0) *
                                         std::cos(2.0 * std::numbers::pi * 5.0 * t));
      value += std::sin(phase) + 0.3 * std::sin(2.0 * phase);
    }
    signal.left[index] = static_cast<float>(0.12 * fade * value);
    signal.right[index] = signal.left[index];
  }
  return signal;
}

} // namespace warp_fixtures

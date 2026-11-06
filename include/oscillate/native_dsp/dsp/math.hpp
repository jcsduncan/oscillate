#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

// The small numeric helpers every native synth, effect and their tests share:
// decibels, windows, the Butterworth Q, the Nyquist guard and seeded
// randomness. Each is defined once here so the engines cannot drift apart.
namespace oscillate::native_dsp::dsp {

// Filter corners, partials and sweeps stay below this fraction of the rate
// they run at, clear of the bilinear warp near Nyquist.
inline constexpr double nyquist_guard = 0.45;

// On/off switches, toggles and model changes fade over this long, so a switch
// never clicks.
inline constexpr double switch_fade_seconds = 0.02;

// The Q of a second-order Butterworth section: maximally flat.
inline constexpr double butterworth_q = std::numbers::sqrt2 / 2.0;

// What a level of zero reads in decibels.
inline constexpr double silent_db = -200.0;

[[nodiscard]] inline double db_to_gain(double db) noexcept { return std::pow(10.0, db / 20.0); }

// An amplitude in decibels; at or below `threshold` it reads `floor_db`.
[[nodiscard]] inline double gain_to_db(double gain, double floor_db = silent_db,
                                       double threshold = 0.0) noexcept {
  return gain > threshold ? 20.0 * std::log10(gain) : floor_db;
}

// A power (squared amplitude) in decibels; at or below `threshold` it reads
// `floor_db`.
[[nodiscard]] inline double power_to_db(double power, double floor_db = silent_db,
                                        double threshold = 0.0) noexcept {
  return power > threshold ? 10.0 * std::log10(power) : floor_db;
}

// The coefficient of the exponential one-pole y += a (x - y) with its corner
// at `hz`.
[[nodiscard]] inline double one_pole_coefficient(double hz, double rate) noexcept {
  return 1.0 - std::exp(-2.0 * std::numbers::pi * hz / rate);
}

// The Hann window at `position` of a cycle `span` long: 0 at both ends of the
// cycle and 1 in its middle. Pass the frame count for a periodic window and
// one less for a symmetric one.
[[nodiscard]] inline double hann(double position, double span) noexcept {
  return 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * position / span);
}

// A raised cosine from 0 at x = 0 to 1 at x = 1, flat at both ends: a fade
// or gate edge with no corner to click.
[[nodiscard]] inline double raised_cosine(double x) noexcept {
  return 0.5 - 0.5 * std::cos(std::numbers::pi * x);
}

// ---- Seeded randomness -----------------------------------------------------------------

// SplitMix64 (Steele, Lea and Flood 2014): its increment, the golden gamma,
// and its finaliser, a stateless 64-bit hash.
inline constexpr std::uint64_t golden_gamma = 0x9E37'79B9'7F4A'7C15ULL;

[[nodiscard]] constexpr std::uint64_t splitmix64_finalise(std::uint64_t x) noexcept {
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

// One SplitMix64 step from `x`: a deterministic hash of a key or seed.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
  return splitmix64_finalise(x + golden_gamma);
}

// The top 53 bits of a hash as a double in [0, 1).
inline constexpr unsigned unit_shift = 11U; // 64 - 53 mantissa bits.
inline constexpr double unit_scale = 0x1.0p-53;
[[nodiscard]] constexpr double unit_from_bits(std::uint64_t bits) noexcept {
  return static_cast<double>(bits >> unit_shift) * unit_scale;
}

// xorshift64*: a fast, deterministic, allocation-free generator. The state
// must not be zero.
class Xorshift64Star {
public:
  constexpr Xorshift64Star() noexcept = default;
  // Seeded from a small integer, spread by the golden gamma.
  constexpr explicit Xorshift64Star(std::uint64_t seed) noexcept
      : state_(seed * golden_gamma + 1U) {}
  // Starts from exactly `state`, which must not be zero.
  [[nodiscard]] static constexpr Xorshift64Star from_state(std::uint64_t state) noexcept {
    Xorshift64Star generator;
    generator.state_ = state;
    return generator;
  }

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return state_ * 0x2545'F491'4F6C'DD1DULL;
  }
  // [0, 1)
  [[nodiscard]] constexpr double uniform() noexcept { return unit_from_bits(next()); }
  // [-1, 1)
  [[nodiscard]] constexpr double bipolar() noexcept { return 2.0 * uniform() - 1.0; }
  // Unit-variance normal (Box-Muller).
  [[nodiscard]] double gaussian() noexcept {
    const double radius = std::sqrt(-2.0 * std::log(1.0 - uniform()));
    return radius * std::cos(2.0 * std::numbers::pi * uniform());
  }
  // A uniform index below `count`, which must be positive.
  [[nodiscard]] std::size_t index(std::size_t count) noexcept {
    return std::min(count - 1, static_cast<std::size_t>(uniform() * static_cast<double>(count)));
  }

private:
  std::uint64_t state_ = 1U;
};

} // namespace oscillate::native_dsp::dsp

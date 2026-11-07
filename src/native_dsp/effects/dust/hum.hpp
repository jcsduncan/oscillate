#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vector>

// Dust's hum layer (ADR-0069): mains hum and buzz from band-limited
// single-cycle tables, buzz's fizz, and a slow waver.
namespace oscillate::native_dsp::effects::dust::detail {

// One cycle of each waveform, band-limited by construction: pure mains hum
// (the fundamental with a few weak harmonics) and buzz (a rectifier's and a
// clipped transformer's harmonics up to hum_harmonics, phase-aligned so the
// cycle is peaky). Width decorrelates the harmonics from the third up: the
// right channel trades them for the same harmonics at other phases, so the
// fundamental and second stay centred.
inline constexpr std::size_t hum_table_size = 4'096;
inline constexpr std::size_t hum_harmonics = 120;
inline constexpr std::size_t hum_wide_from = 3;
inline constexpr std::array<double, 11> hum_pure_harmonics{1.0,  0.18, 0.32, 0.05, 0.12, 0.02,
                                                           0.06, 0.0,  0.03, 0.0,  0.015};
inline constexpr double hum_buzz_odd_law = 0.75;
inline constexpr double hum_buzz_even_level = 0.6;
inline constexpr double hum_buzz_even_law = 0.9;
inline constexpr double hum_buzz_second = 0.8; // magnetostriction at twice the mains
inline constexpr std::uint64_t hum_phase_seed = 0x48554DULL;
// Tone: a low-pass from hum_tone_low_hz to hum_tone_low_hz * hum_tone_ratio.
inline constexpr double hum_tone_low_hz = 150.0;
inline constexpr double hum_tone_ratio = 50.0;
// Buzz's fizz: noise gated by |sin|^12 at the mains' peaks, high-passed.
inline constexpr double hum_fizz = 0.6;
inline constexpr double hum_fizz_hp_hz = 2'000.0;
// E[sin^24] = C(24, 12) / 2^24: the fizz gate's mean power.
inline constexpr double hum_fizz_gate_power = 2'704'156.0 / 16'777'216.0;
// A slow waver in level.
inline constexpr double hum_waver = 0.08;
inline constexpr double hum_waver_seconds = 4.0;

struct HumTables {
  std::vector<double> pure;
  std::vector<double> pure_high;
  std::vector<double> pure_other;
  std::vector<double> buzz;
  std::vector<double> buzz_high;
  std::vector<double> buzz_other;
  // Harmonic amplitudes, scaled so a full cycle has unit mean power.
  std::array<double, hum_harmonics> pure_amp{};
  std::array<double, hum_harmonics> buzz_amp{};
};

// Built once, on first use (control only).
[[nodiscard]] const HumTables& hum_tables();

class Hum {
public:
  void prepare(LayerFormat format);
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, LayerShape shape) noexcept;
  [[nodiscard]] Pair next(std::uint64_t frame) noexcept {
    // Phase from the frame: exact however long it plays.
    const double phase = std::fmod(static_cast<double>(frame) * hz_, rate_) / rate_;
    const double position = phase * static_cast<double>(hum_table_size);
    const auto i = std::min(static_cast<std::size_t>(position), hum_table_size - 1);
    const double f = position - static_cast<double>(i);
    const auto read = [&](const std::vector<double>& table) {
      return table[i] + f * (table[i + 1] - table[i]);
    };
    const auto& tb = *tables_;
    const double pure = read(tb.pure);
    const double buzz = read(tb.buzz);
    double left = pure_ * pure + buzz_ * buzz;
    double right = pure_ * (pure - keep_ * read(tb.pure_high) + other_ * read(tb.pure_other)) +
                   buzz_ * (buzz - keep_ * read(tb.buzz_high) + other_ * read(tb.buzz_other));
    if (fizz_ > 0.0) {
      const double s = std::sin(two_pi * phase);
      const double s2 = s * s;
      const double s4 = s2 * s2;
      const double gate = s4 * s4 * s4 * fizz_;
      left += fizz_hp_[0].high(rng_.white() * gate);
      right += fizz_hp_[1].high(rng_.white() * gate);
    }
    const double g = gain_ * (1.0 + hum_waver * waver_.next(rng_, hum_waver_seconds, rate_));
    return {low_[0].low(left) * g, low_[1].low(right) * g};
  }

private:
  double rate_ = nominal_rate;
  double reference_ = 0.0;
  const HumTables* tables_ = nullptr;
  Rng rng_;
  std::array<Svf, 2> low_{};
  std::array<OnePole, 2> fizz_hp_{};
  Drift waver_;
  double hz_ = mains_hz[0];
  double pure_ = 1.0;
  double buzz_ = 0.0;
  double fizz_ = 0.0;
  double keep_ = 0.0;
  double other_ = 0.0;
  double tone_ = -1.0;
  double buzz_set_ = -1.0;
  double hz_set_ = -1.0;
  double gain_ = 0.0;
};

} // namespace oscillate::native_dsp::effects::dust::detail

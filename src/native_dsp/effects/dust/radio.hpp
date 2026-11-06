#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's radio layer (ADR-0069): AM static and sferics, heterodyne whistles
// between seeded stations, slow fading, and a tempo-locked gate.
namespace oscillate::native_dsp::effects::dust::detail {

// AM static: noise through a broad band (Tone moves it) inside the AM
// channel's 4.5 kHz, with sferics (atmospheric crackles, Pareto sizes)
// through the same band, fading slowly in and out. The levels carry the
// 3.2 dB the channel's low-pass takes out of the band (measured).
inline constexpr double radio_band_low_hz = 900.0;
inline constexpr double radio_band_ratio = 4.0;
inline constexpr double radio_band_q = 0.6;
inline constexpr double radio_channel_hz = 4'500.0;
inline constexpr double radio_noise_db = 1.2;
inline constexpr double radio_sferic_db = -2.8;
inline constexpr double radio_sferics_a_second = 25.0;
inline constexpr double radio_sferic_alpha = 2.0;
inline constexpr double radio_sferic_cap = 20.0;
inline constexpr double radio_fade_db = 9.0;
inline constexpr double radio_fade_seconds = 5.0;
// Heterodyne whistles: stations at seeded places on the dial; between them
// each whistles at its distance from Tuning, fainter as it gets further.
inline constexpr std::size_t radio_stations = 3;
inline constexpr double radio_whistle_low_hz = 60.0;
inline constexpr double radio_whistle_span_hz = 12'000.0;
inline constexpr double radio_whistle_reach = 0.12;
inline constexpr double radio_whistle_db = -4.8;
inline constexpr double radio_wobble_hz = 3.0;
inline constexpr double radio_wobble_seconds = 1.5;
// The gate: each step of 1/4, 1/8 or 1/16 (gate_beats) opens (radio_gate_open
// of them, by the seed) with a radio_gate_attack_s attack, holds for
// gate_hold of the step and closes over radio_gate_release_s, down to
// radio_gate_floor.
inline constexpr double radio_gate_open = 0.8;
inline constexpr double radio_gate_attack_s = 0.002;
inline constexpr double radio_gate_release_s = 0.006;
inline constexpr double radio_gate_floor = 0.1;

class Radio {
public:
  void prepare(double rate, double reference);
  // The dial: where the stations are, from the seed alone.
  void tune(std::uint64_t seed_key) noexcept;
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double tone, double width, double steps,
           double seconds_per_beat) noexcept;
  [[nodiscard]] Pair next(double beat) noexcept {
    double sferic = 0.0;
    if (rng_.unit() < sferic_chance_) {
      sferic = sferic_gain_ * rng_.sign() *
               std::min(std::pow(rng_.open_unit(), -1.0 / radio_sferic_alpha), radio_sferic_cap);
    }
    const double a = channel_[0].low(band_[0].band(noise_gain_ * rng_.white() + sferic));
    const double b = channel_[1].low(band_[1].band(noise_gain_ * rng_.white() + sferic));
    Pair out = width_.place(a, b);
    double whistle = 0.0;
    for (std::size_t i = 0; i < radio_stations; ++i) {
      whistle += whistle_amp_[i] * whistles_[i].next();
      (void)wobble_[i].next(rng_, radio_wobble_seconds, rate_);
    }
    (void)fade_.next(rng_, radio_fade_seconds, rate_);
    const double g = fade_ramp_.next() * gate(beat);
    out.left = (out.left + whistle) * g;
    out.right = (out.right + whistle) * g;
    return out;
  }

private:
  [[nodiscard]] double gate(double beat) const noexcept {
    if (step_beats_ <= 0.0) {
      return 1.0;
    }
    const double steps = beat / step_beats_;
    const double index = std::floor(steps);
    const double within = (steps - index) * step_seconds_;
    const auto key = static_cast<std::uint64_t>(static_cast<std::int64_t>(index));
    if (unit_from_bits(splitmix64(gate_key_ ^ key)) >= radio_gate_open) {
      return radio_gate_floor;
    }
    const double attack = std::clamp(within / radio_gate_attack_s, 0.0, 1.0);
    const double release =
        std::clamp((gate_hold * step_seconds_ - within) / radio_gate_release_s + 1.0, 0.0, 1.0);
    return radio_gate_floor + (1.0 - radio_gate_floor) * attack * release;
  }

  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<Svf, 2> band_{};
  std::array<Svf, 2> channel_{};
  std::array<Osc, radio_stations> whistles_{};
  std::array<Drift, radio_stations> wobble_{};
  std::array<double, radio_stations> whistle_amp_{};
  std::array<double, radio_stations> stations_{};
  Drift fade_;
  dsp::Ramp fade_ramp_;
  Width width_;
  double tone_ = -1.0;
  double power_ = 1.0;
  double noise_gain_ = 0.0;
  double sferic_gain_ = 0.0;
  double sferic_chance_ = 0.0;
  double fade_gain_ = 1.0;
  double step_beats_ = 0.0;
  double step_seconds_ = 0.0;
  std::uint64_t gate_key_ = 0;
  bool jump_next_ = true;
};

} // namespace oscillate::native_dsp::effects::dust::detail

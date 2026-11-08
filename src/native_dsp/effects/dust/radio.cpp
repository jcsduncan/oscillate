#include "effects/dust/radio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {
namespace {

// Salts that keep the dial's and the gate's sequences their own.
constexpr std::uint64_t dial_salt = 0xD1A15ULL;
constexpr std::uint64_t gate_salt = 0x6A7E5ULL;
// Decibels in a decade of power.
constexpr double power_decade_db = 10.0;

// The mean power of 10^(-F (1 + d) / 20) over a roughly uniform d in
// [-1, 1]: so the fading layer's average sits at the reference.
[[nodiscard]] double fade_compensation(double depth_db) noexcept {
  const double x = depth_db * ln10 / power_decade_db;
  return 1.0 / std::sqrt((1.0 - std::exp(-x)) / x);
}

} // namespace

void Radio::prepare(LayerFormat format) {
  const double rate = format.rate;
  const double reference = format.reference;
  rate_ = rate;
  reference_ = reference;
  sferic_chance_ = radio_sferics_a_second / rate;
  for (auto& f : channel_) {
    f.set(radio_channel_hz, butterworth_q, rate);
  }
  fade_gain_ = fade_compensation(radio_fade_db);
}

void Radio::tune(std::uint64_t seed_key) noexcept {
  Rng rng;
  rng.seed(seed_key ^ dial_salt);
  for (auto& station : stations_) {
    station = rng.unit();
  }
  gate_key_ = splitmix64(seed_key ^ gate_salt);
}

void Radio::reset() noexcept {
  for (std::size_t c = 0; c < channels; ++c) {
    band_[c].reset();
    channel_[c].reset();
  }
  for (auto& w : whistles_) {
    w.reset();
  }
  for (auto& d : wobble_) {
    d.reset();
  }
  fade_.reset();
  jump_next_ = true;
}

void Radio::set(std::span<const double> v, LayerShape shape, LayerTiming timing) noexcept {
  const double tone = shape.tone;
  const double width = shape.width;
  const double steps = timing.steps;
  const double seconds_per_beat = timing.seconds_per_beat;
  if (tone != tone_) {
    tone_ = tone;
    const double centre = radio_band_low_hz * std::pow(radio_band_ratio, tone);
    for (auto& f : band_) {
      f.set(centre, radio_band_q, rate_);
    }
    const std::array chain{band2(centre, radio_band_q), low2(radio_channel_hz, butterworth_q)};
    power_ = noise_power(chain, rate_);
    noise_gain_ = reference_ * gain_of(radio_noise_db) / std::sqrt(power_);
    sferic_gain_ = reference_ * gain_of(radio_sferic_db) /
                   std::sqrt(sferic_chance_ *
                             pareto_square_mean(radio_sferic_alpha, radio_sferic_cap) * power_);
  }
  const double tuning = v[p::radio_tuning];
  for (std::size_t i = 0; i < radio_stations; ++i) {
    const double distance = std::fabs(tuning - stations_[i]);
    const double hz = radio_whistle_low_hz + radio_whistle_span_hz * distance +
                      radio_wobble_hz * wobble_[i].value();
    whistles_[i].set(hz, rate_);
    whistles_[i].normalise();
    whistle_amp_[i] =
        reference_ * gain_of(radio_whistle_db) * std::exp(-distance / radio_whistle_reach);
  }
  const double fade = gain_of(-radio_fade_db * (1.0 + fade_.value()) * 0.5) * fade_gain_;
  fade_ramp_.to(fade, steps, jump_next_);
  jump_next_ = false;
  const auto gate = choice_of(v[p::radio_gate], gate_beats.size());
  step_beats_ = gate_beats[gate];
  step_seconds_ = step_beats_ * seconds_per_beat;
  width_.set(width);
}

} // namespace oscillate::native_dsp::effects::dust::detail

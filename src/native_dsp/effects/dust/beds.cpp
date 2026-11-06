#include "effects/dust/beds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <numbers>

namespace oscillate::native_dsp::effects::dust::detail {
namespace {

// Salt that keeps the machine's build its own sequence.
constexpr std::uint64_t machine_salt = 0x3AC41E5ULL;

} // namespace

void Wind::prepare(double rate, double reference) {
  rate_ = rate;
  reference_ = reference;
  for (auto& f : buffet_) {
    f.set(wind_buffet_hz, butterworth_q, rate);
  }
}

void Wind::reset() noexcept {
  for (std::size_t c = 0; c < channels; ++c) {
    for (auto& f : bands_[c]) {
      f.reset();
    }
    buffet_[c].reset();
    whistle_[c].reset();
  }
  for (auto& d : wander_) {
    d.reset();
  }
  gust_.reset();
  whistle_wander_.reset();
  jump_next_ = true;
}

void Wind::set(std::span<const double> v, double tone, double width, double steps,
               bool jump) noexcept {
  const double strength = v[p::wind_strength];
  const double gusts = v[p::wind_gusts];
  gust_seconds_ = wind_gust_min_seconds + wind_gust_span_seconds * (1.0 - gusts);
  const double blow =
      std::clamp(strength * (1.0 + wind_gust_depth * gusts * gust_.value()), 0.0, 1.0);
  const double colour = std::pow(2.0, wind_tone_octaves * (tone - control_centre));
  const double q = wind_q_min + wind_q_span * blow;
  double power = 0.0;
  for (std::size_t i = 0; i < wind_band_hz.size(); ++i) {
    const double hz = wind_band_hz[i] * (wind_band_low + wind_band_span * blow) *
                      std::pow(2.0, wind_wander_octaves * wander_[i].value()) * colour;
    for (std::size_t c = 0; c < channels; ++c) {
      bands_[c][i].set(hz, q, rate_);
    }
    power += wind_band_level[i] * wind_band_level[i] * band_power(hz, q, rate_);
  }
  power += wind_buffet_level * wind_buffet_level * pi * butterworth_q * wind_buffet_hz / rate_;
  const double x = std::clamp((blow - wind_whistle_from) / wind_whistle_over, 0.0, 1.0);
  whistle_level_ = wind_whistle_level * x * x * (3.0 - 2.0 * x);
  const double whistle_hz = wind_whistle_hz * (wind_whistle_low + wind_whistle_span * blow) *
                            std::pow(2.0, wind_whistle_wander_octaves * whistle_wander_.value()) *
                            colour;
  for (auto& f : whistle_) {
    f.set(whistle_hz, wind_whistle_q, rate_);
  }
  power += whistle_level_ * whistle_level_ * band_power(whistle_hz, wind_whistle_q, rate_);
  const double loud = std::pow(blow / def(p::wind_strength), wind_loudness_law);
  gain_.to(reference_ * loud / std::sqrt(power), steps, jump || jump_next_);
  jump_next_ = false;
  width_.set(width);
}

void Room::prepare(double rate, double reference) {
  rate_ = rate;
  reference_ = reference;
  for (std::size_t c = 0; c < channels; ++c) {
    brown_[c].set(room_brown_hz, rate);
    sub_[c].set(room_sub_hz, rate);
    air_high_[c].set(room_air_hp_hz, rate);
    air_low_[c].set(room_air_lp_hz, butterworth_q, rate);
  }
  const std::array air{high1(room_air_hp_hz), low2(room_air_lp_hz, butterworth_q)};
  air_gain_ = reference * gain_of(room_air_db) / std::sqrt(noise_power(air, rate));
}

void Room::reset() noexcept {
  for (std::size_t c = 0; c < channels; ++c) {
    brown_[c].reset();
    sub_[c].reset();
    corner_[c].reset();
    air_high_[c].reset();
    air_low_[c].reset();
    for (auto& m : modes_[c]) {
      m.reset();
    }
  }
  for (auto& d : mode_move_) {
    d.reset();
  }
  waver_.reset();
}

void Room::set(std::span<const double> v, double tone, double width) noexcept {
  const double size = v[p::room_size];
  if (size != size_ || tone != tone_) {
    size_ = size;
    tone_ = tone;
    const double corner = room_corner_hz * std::pow(room_corner_ratio, size) *
                          std::pow(2.0, room_tone_octaves * (tone - control_centre));
    double mode_power = 0.0;
    for (std::size_t i = 0; i < room_mode_ratios.size(); ++i) {
      const double hz = room_mode_ratios[i] * (room_mode_low_hz - room_mode_size_hz * size);
      for (std::size_t c = 0; c < channels; ++c) {
        modes_[c][i].set(hz, room_mode_q, rate_);
      }
      mode_power += band_power(hz, room_mode_q, rate_);
    }
    for (auto& f : corner_) {
      f.set(corner, room_corner_q, rate_);
    }
    const std::array chain{low1(room_brown_hz), low2(corner, room_corner_q), high1(room_sub_hz)};
    main_gain_ = reference_ * gain_of(room_main_db) / std::sqrt(noise_power(chain, rate_));
    mode_gain_ = reference_ * gain_of(room_mode_db) / std::sqrt(mode_power);
  }
  width_.set(width);
}

void Machine::prepare(double rate, double reference) {
  rate_ = rate;
  reference_ = reference;
  for (auto& f : gurgle_) {
    f.set(fridge_gurgle_hz, fridge_gurgle_q, rate);
  }
  for (auto& f : air_high_) {
    f.set(server_air_hp_hz, rate);
  }
  click_.set(fridge_click_hz, rate);
  thump_.set(fridge_thump_hz, rate);
  click_decay_ = std::exp(-1.0 / (fridge_click_s * rate));
  thump_decay_ = std::exp(-1.0 / (fridge_thump_s * rate));
  spin_ = 1.0 - std::exp(-1.0 / (fridge_spin_s * rate));
}

void Machine::build(std::uint64_t seed_key) noexcept {
  Rng rng;
  rng.seed(seed_key ^ machine_salt);
  for (auto& s : spread_) {
    s = rng.bipolar();
  }
  cycle_offset_ = rng.unit();
}

void Machine::reset() noexcept {
  for (auto& o : tones_) {
    o.reset();
  }
  for (std::size_t c = 0; c < channels; ++c) {
    air_[c].reset();
    air_high_[c].reset();
    gurgle_[c].reset();
  }
  wobble_.reset();
  gurgle_move_.reset();
  whine_wobble_.reset();
  whine_.reset();
  click_.reset();
  thump_.reset();
  click_level_ = 0.0;
  thump_level_ = 0.0;
  motor_ = -1.0;
  running_ = -1;
}

void Machine::set(std::span<const double> v, double tone, double width) noexcept {
  const auto type = std::min(static_cast<int>(v[p::machine_type]), static_cast<int>(t::servers));
  const double speed = v[p::machine_speed];
  const auto mains = choice_of(v[p::hum_mains], mains_hz.size());
  const double colour = std::pow(2.0, machine_tone_octaves * (tone - control_centre));
  const double wobble = 1.0 + machine_wobble * wobble_.value();
  type_ = type;
  tone_count_ = 0;
  double tone_power = 0.0;
  const auto add_tone = [&](double hz, double amplitude, double pan) {
    if (tone_count_ < machine_tones) {
      tones_[tone_count_].set(hz, rate_);
      tones_[tone_count_].normalise();
      amp_[tone_count_] = amplitude;
      pans_[tone_count_] = pan_gains(pan);
      tone_power += sine_power * amplitude * amplitude;
      ++tone_count_;
    }
  };
  double air_power = 1.0;
  if (type != config_type_ || speed != config_speed_ || tone != config_tone_ ||
      mains != config_mains_) {
    config_type_ = type;
    config_speed_ = speed;
    config_tone_ = tone;
    config_mains_ = mains;
    config_dirty_ = true;
  }
  double tones_db = fan_tones_db;
  double air_db = fan_air_db;
  if (type == t::fan) {
    blade_hz_ = (fan_low_hz + fan_span_hz * speed) * wobble;
    for (std::size_t h = 0; h < fan_harmonics.size(); ++h) {
      add_tone(blade_hz_ * static_cast<double>(h + 1), fan_harmonics[h], 0.0);
    }
    if (config_dirty_) {
      const double hz = (fan_air_low_hz + fan_air_span_hz * speed) * colour;
      for (auto& f : air_) {
        f.set(hz, fan_air_q, rate_);
      }
      air_power_ = band_power(hz, fan_air_q, rate_) * (1.0 + sine_power * fan_pulse * fan_pulse);
    }
    air_power = air_power_;
  } else if (type == t::fridge) {
    const double hz = fridge_mains_multiple * mains_hz[mains] * wobble;
    for (std::size_t h = 0; h + 1 < fridge_harmonics.size(); ++h) {
      add_tone(hz * static_cast<double>(h + 1), fridge_harmonics[h], 0.0);
    }
    add_tone(hz / fridge_mains_multiple, fridge_harmonics.back(), 0.0);
    period_s_ = fridge_period_max_s - fridge_period_span_s * speed;
    tones_db = fridge_tones_db;
    air_db = fridge_gurgle_db;
    air_power = band_power(fridge_gurgle_hz, fridge_gurgle_q, rate_);
  } else {
    const double base = server_low_hz + server_span_hz * speed;
    for (std::size_t k = 0; k < server_fans; ++k) {
      const double pan =
          width * (2.0 * static_cast<double>(k) / static_cast<double>(server_fans - 1) - 1.0);
      for (std::size_t h = 0; h < server_harmonics.size(); ++h) {
        add_tone(base * (1.0 + server_spread * spread_[k]) * wobble * static_cast<double>(h + 1),
                 server_harmonics[h], pan);
      }
    }
    if (config_dirty_) {
      const double hz = std::min((server_air_low_hz + server_air_span_hz * speed) * colour,
                                 nyquist_guard * rate_);
      for (auto& f : air_) {
        f.set(hz, butterworth_q, rate_);
      }
      const std::array chain{high1(server_air_hp_hz), low2(hz, butterworth_q)};
      air_power_ = noise_power(chain, rate_);
    }
    air_power = air_power_;
    tones_db = server_fans_db;
    air_db = server_air_db;
    whine_hz_ = server_whine_hz + server_whine_span_hz * tone;
  }
  config_dirty_ = false;
  // The fridge is calibrated over its whole cycle.
  const double duty = type == t::fridge
                          ? std::sqrt(fridge_duty + (1.0 - fridge_duty) * fridge_rest * fridge_rest)
                          : 1.0;
  tone_gain_ = reference_ * gain_of(tones_db) / std::sqrt(std::max(tone_power, power_floor)) / duty;
  air_gain_ = reference_ * gain_of(air_db) / std::sqrt(air_power) / duty;
  if (type == t::servers) {
    whine_.set(whine_hz_ + server_whine_wobble_hz * whine_wobble_.value(), rate_);
    whine_.normalise();
    whine_gain_ = reference_ * gain_of(server_whine_db) * std::numbers::sqrt2;
  }
  click_gain_ = reference_ * gain_of(fridge_click_db);
  width_.set(width);
}

} // namespace oscillate::native_dsp::effects::dust::detail

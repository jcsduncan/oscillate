#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's steady beds (ADR-0069): wind, room tone and machines.
namespace oscillate::native_dsp::effects::dust::detail {

// ---- Wind -----------------------------------------------------------------------------------

// Three resonant bands wandering about wind_band_hz (higher and narrower as
// the wind blows harder), low buffeting, and at strong settings a whistle:
// a very narrow band. Gusts swell the strength slowly.
inline constexpr std::array<double, 3> wind_band_hz{350.0, 800.0, 1'700.0};
inline constexpr std::array<double, 3> wind_band_level{1.0, 0.7, 0.45};
inline constexpr double wind_band_low = 0.6;
inline constexpr double wind_band_span = 0.8;
inline constexpr double wind_wander_octaves = 0.6;
inline constexpr double wind_wander_seconds = 2.5;
inline constexpr double wind_tone_octaves = 2.0;
inline constexpr double wind_q_min = 1.5;
inline constexpr double wind_q_span = 4.0;
inline constexpr double wind_buffet_hz = 120.0;
inline constexpr double wind_buffet_level = 0.6;
inline constexpr double wind_gust_depth = 0.6;
inline constexpr double wind_gust_min_seconds = 2.0;
inline constexpr double wind_gust_span_seconds = 6.0;
inline constexpr double wind_loudness_law = 1.5;
inline constexpr double wind_whistle_hz = 1'500.0;
inline constexpr double wind_whistle_low = 0.7;
inline constexpr double wind_whistle_span = 0.6;
inline constexpr double wind_whistle_q = 25.0;
inline constexpr double wind_whistle_from = 0.55;
inline constexpr double wind_whistle_over = 0.4;
inline constexpr double wind_whistle_level = 1.5;
inline constexpr double wind_whistle_seconds = 3.0;
// The whistle wanders by up to this many octaves.
inline constexpr double wind_whistle_wander_octaves = 0.5;

class Wind {
public:
  void prepare(double rate, double reference);
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double tone, double width, double steps, bool jump) noexcept;
  [[nodiscard]] Pair next() noexcept {
    for (auto& d : wander_) {
      (void)d.next(rng_, wind_wander_seconds, rate_);
    }
    (void)gust_.next(rng_, gust_seconds_, rate_);
    (void)whistle_wander_.next(rng_, wind_whistle_seconds, rate_);
    std::array<double, channels> y{};
    for (std::size_t c = 0; c < channels; ++c) {
      const double n = rng_.white();
      double sum = wind_buffet_level * buffet_[c].low(n);
      for (std::size_t i = 0; i < wind_band_hz.size(); ++i) {
        sum += wind_band_level[i] * bands_[c][i].band(n);
      }
      if (whistle_level_ > 0.0) {
        sum += whistle_level_ * whistle_[c].band(n);
      }
      y[c] = sum;
    }
    const double g = gain_.next();
    return width_.place(y[0] * g, y[1] * g);
  }

private:
  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<std::array<Svf, wind_band_hz.size()>, channels> bands_{};
  std::array<Svf, 2> buffet_{};
  std::array<Svf, 2> whistle_{};
  std::array<Drift, wind_band_hz.size()> wander_{};
  Drift gust_;
  Drift whistle_wander_;
  dsp::Ramp gain_;
  Width width_;
  double gust_seconds_ = wind_gust_min_seconds;
  double whistle_level_ = 0.0;
  bool jump_next_ = true;
};

// ---- Room tone ------------------------------------------------------------------------------

// A quiet, slightly moving bed of air: brownish noise below a corner Size
// lowers (Tone moves it), faint room modes (Size spaces them), and a
// whisper of air.
inline constexpr double room_brown_hz = 20.0;
inline constexpr double room_sub_hz = 25.0;
inline constexpr double room_corner_hz = 900.0;
inline constexpr double room_corner_ratio = 0.25;
inline constexpr double room_corner_q = 0.6;
inline constexpr double room_tone_octaves = 2.0;
inline constexpr std::array<double, 3> room_mode_ratios{1.0, 1.41, 1.87};
inline constexpr double room_mode_low_hz = 110.0;
inline constexpr double room_mode_size_hz = 60.0;
inline constexpr double room_mode_q = 10.0;
inline constexpr double room_mode_move = 0.5;
inline constexpr double room_mode_seconds = 6.0;
inline constexpr double room_air_hp_hz = 2'000.0;
inline constexpr double room_air_lp_hz = 8'000.0;
inline constexpr double room_main_db = -1.0;
inline constexpr double room_mode_db = -9.0;
inline constexpr double room_air_db = -20.0;
inline constexpr double room_waver = 0.12;
inline constexpr double room_waver_seconds = 8.0;

class Room {
public:
  void prepare(double rate, double reference);
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double tone, double width) noexcept;
  [[nodiscard]] Pair next() noexcept {
    std::array<double, channels> y{};
    std::array<double, room_mode_ratios.size()> move{};
    for (std::size_t i = 0; i < move.size(); ++i) {
      move[i] = 1.0 + room_mode_move * mode_move_[i].next(rng_, room_mode_seconds, rate_);
    }
    for (std::size_t c = 0; c < channels; ++c) {
      const double n = rng_.white();
      double sum = main_gain_ * sub_[c].high(corner_[c].low(brown_[c].low(n)));
      for (std::size_t i = 0; i < move.size(); ++i) {
        sum += mode_gain_ * move[i] * modes_[c][i].band(n);
      }
      sum += air_gain_ * air_low_[c].low(air_high_[c].high(rng_.white()));
      y[c] = sum;
    }
    const double g = 1.0 + room_waver * waver_.next(rng_, room_waver_seconds, rate_);
    return width_.place(y[0] * g, y[1] * g);
  }

private:
  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<OnePole, 2> brown_{};
  std::array<OnePole, 2> sub_{};
  std::array<Svf, 2> corner_{};
  std::array<OnePole, 2> air_high_{};
  std::array<Svf, 2> air_low_{};
  std::array<std::array<Svf, room_mode_ratios.size()>, channels> modes_{};
  std::array<Drift, room_mode_ratios.size()> mode_move_{};
  Drift waver_;
  Width width_;
  double size_ = -1.0;
  double tone_ = -1.0;
  double main_gain_ = 0.0;
  double mode_gain_ = 0.0;
  double air_gain_ = 0.0;
};

// ---- Machine --------------------------------------------------------------------------------

// Fan: a blade-pass tone and its harmonics over broadband air pulsing at
// the blade rate. Fridge: the compressor's hum at twice the mains with its
// harmonics, cycling on and off (relay clicks at each change) over a
// quieter coil fan and a gurgle. Servers: many fans a little apart
// (beating), broadband air and a faint high whine. Speed scales them.
inline constexpr std::size_t machine_tones = 13;
inline constexpr std::array<double, 4> fan_harmonics{1.0, 0.5, 0.3, 0.15};
inline constexpr double fan_low_hz = 30.0;
inline constexpr double fan_span_hz = 110.0;
inline constexpr double fan_air_low_hz = 600.0;
inline constexpr double fan_air_span_hz = 1'400.0;
inline constexpr double fan_air_q = 0.7;
inline constexpr double fan_pulse = 0.35;
inline constexpr double fan_tones_db = -5.0;
inline constexpr double fan_air_db = -2.0;
inline constexpr std::array<double, 5> fridge_harmonics{1.0, 0.4, 0.25, 0.1,
                                                        0.15}; // last: the mains
inline constexpr double fridge_period_max_s = 40.0;
inline constexpr double fridge_period_span_s = 30.0;
inline constexpr double fridge_duty = 0.65;
inline constexpr double fridge_rest = 0.15;
inline constexpr double fridge_spin_s = 0.6;
inline constexpr double fridge_tones_db = -2.0;
inline constexpr double fridge_gurgle_hz = 320.0;
inline constexpr double fridge_gurgle_q = 3.0;
inline constexpr double fridge_gurgle_db = -12.0;
inline constexpr double fridge_gurgle_seconds = 0.7;
inline constexpr double fridge_click_hz = 2'200.0;
inline constexpr double fridge_click_s = 0.003;
inline constexpr double fridge_thump_hz = 80.0;
inline constexpr double fridge_thump_s = 0.025;
inline constexpr double fridge_click_db = 0.0;
inline constexpr std::size_t server_fans = 6;
inline constexpr std::array<double, 2> server_harmonics{1.0, 0.35};
inline constexpr double server_low_hz = 90.0;
inline constexpr double server_span_hz = 160.0;
inline constexpr double server_spread = 0.06;
inline constexpr double server_air_hp_hz = 150.0;
inline constexpr double server_air_low_hz = 3'000.0;
inline constexpr double server_air_span_hz = 5'000.0;
inline constexpr double server_air_db = -1.5;
inline constexpr double server_fans_db = -8.0;
inline constexpr double server_whine_hz = 9'000.0;
inline constexpr double server_whine_span_hz = 5'000.0;
inline constexpr double server_whine_db = -26.0;
inline constexpr double server_whine_wobble_hz = 20.0;
inline constexpr double machine_tone_octaves = 2.0;
inline constexpr double machine_wobble = 0.005;
inline constexpr double machine_wobble_seconds = 3.0;
// The fridge's compressor hums at this multiple of the mains.
inline constexpr double fridge_mains_multiple = 2.0;

class Machine {
public:
  void prepare(double rate, double reference);
  // The machine's build: the servers' fans and the fridge's cycle, from
  // the seed alone.
  void build(std::uint64_t seed_key) noexcept;
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double tone, double width) noexcept;
  [[nodiscard]] Pair next(std::int64_t time) noexcept {
    (void)wobble_.next(rng_, machine_wobble_seconds, rate_);
    Pair out;
    double motor = 1.0;
    if (type_ == t::fridge) {
      // The compressor's cycle, from the song position.
      const double period = period_s_ * rate_;
      const double cycle = std::fmod(static_cast<double>(time) / period + cycle_offset_, 1.0);
      const int running = (cycle < 0.0 ? cycle + 1.0 : cycle) < fridge_duty ? 1 : 0;
      if (running_ >= 0 && running != running_) {
        click_level_ = click_gain_;
        thump_level_ = click_gain_;
        click_.reset();
        thump_.reset();
      }
      running_ = running;
      const double target = running != 0 ? 1.0 : fridge_rest;
      if (motor_ < 0.0) {
        motor_ = target;
      }
      motor_ += spin_ * (target - motor_);
      motor = motor_;
    }
    double tonal_left = 0.0;
    double tonal_right = 0.0;
    double blade = 0.0;
    for (std::size_t i = 0; i < tone_count_; ++i) {
      const double y = amp_[i] * tones_[i].next();
      if (i == 0) {
        blade = tones_[i].im;
      }
      tonal_left += y * pans_[i].left;
      tonal_right += y * pans_[i].right;
    }
    tonal_left *= tone_gain_ * motor;
    tonal_right *= tone_gain_ * motor;
    std::array<double, channels> air{};
    if (type_ == t::fan) {
      const double pulse = 1.0 + fan_pulse * blade;
      for (std::size_t c = 0; c < channels; ++c) {
        air[c] = air_[c].band(rng_.white()) * pulse;
      }
    } else if (type_ == t::fridge) {
      const double move = 1.0 + gurgle_move_.next(rng_, fridge_gurgle_seconds, rate_);
      for (std::size_t c = 0; c < channels; ++c) {
        air[c] = gurgle_[c].band(rng_.white()) * move * motor;
      }
    } else {
      for (std::size_t c = 0; c < channels; ++c) {
        air[c] = air_[c].low(air_high_[c].high(rng_.white()));
      }
      (void)whine_wobble_.next(rng_, machine_wobble_seconds, rate_);
    }
    out = width_.place(air[0] * air_gain_, air[1] * air_gain_);
    out.left += tonal_left;
    out.right += tonal_right;
    if (type_ == t::servers) {
      const double whine = whine_gain_ * whine_.next();
      out.left += whine;
      out.right += whine;
    }
    if (click_level_ > silent_level || thump_level_ > silent_level) {
      const double c = click_level_ * click_.next() + thump_level_ * thump_.next();
      click_level_ *= click_decay_;
      thump_level_ *= thump_decay_;
      out.left += c;
      out.right += c;
    }
    return out;
  }

private:
  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<Osc, machine_tones> tones_{};
  std::array<double, machine_tones> amp_{};
  std::array<Pair, machine_tones> pans_{};
  std::size_t tone_count_ = 0;
  std::array<Svf, 2> air_{};
  std::array<OnePole, 2> air_high_{};
  std::array<Svf, 2> gurgle_{};
  std::array<double, server_fans> spread_{};
  Drift wobble_;
  Drift gurgle_move_;
  Drift whine_wobble_;
  Osc whine_;
  Osc click_;
  Osc thump_;
  Width width_;
  int type_ = t::fan;
  int config_type_ = -1;
  double config_speed_ = -1.0;
  double config_tone_ = -1.0;
  std::size_t config_mains_ = 0;
  bool config_dirty_ = true;
  double air_power_ = 1.0;
  double blade_hz_ = fan_low_hz;
  double period_s_ = fridge_period_max_s;
  double cycle_offset_ = 0.0;
  double whine_hz_ = server_whine_hz;
  double tone_gain_ = 0.0;
  double air_gain_ = 0.0;
  double whine_gain_ = 0.0;
  double click_gain_ = 0.0;
  double click_level_ = 0.0;
  double thump_level_ = 0.0;
  double click_decay_ = 0.0;
  double thump_decay_ = 0.0;
  double spin_ = 0.0;
  double motor_ = -1.0;
  int running_ = -1;
};

} // namespace oscillate::native_dsp::effects::dust::detail

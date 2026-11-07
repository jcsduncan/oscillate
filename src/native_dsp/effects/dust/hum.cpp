#include "effects/dust/hum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vector>

namespace oscillate::native_dsp::effects::dust::detail {
namespace {

void add_harmonic(std::vector<double>& table, std::size_t n, double amplitude, double phase) {
  // By rotation: one complex multiply a point.
  const double step = two_pi * static_cast<double>(n) / static_cast<double>(hum_table_size);
  const double cr = std::cos(step);
  const double ci = std::sin(step);
  double re = std::cos(phase);
  double im = std::sin(phase);
  for (std::size_t i = 0; i < hum_table_size; ++i) {
    table[i] += amplitude * im;
    const double r = re * cr - im * ci;
    im = re * ci + im * cr;
    re = r;
  }
}

HumTables build_hum_tables() {
  HumTables tables;
  for (std::size_t n = 1; n <= hum_harmonics; ++n) {
    const double h = static_cast<double>(n);
    tables.pure_amp[n - 1] = n <= hum_pure_harmonics.size() ? hum_pure_harmonics[n - 1] : 0.0;
    tables.buzz_amp[n - 1] = n % 2 == 1 ? std::pow(h, -hum_buzz_odd_law)
                                        : hum_buzz_even_level * std::pow(h, -hum_buzz_even_law);
  }
  tables.buzz_amp[1] += hum_buzz_second;
  for (auto* amp : {&tables.pure_amp, &tables.buzz_amp}) {
    double power = 0.0;
    for (const double a : *amp) {
      power += sine_power * a * a;
    }
    const double scale = 1.0 / std::sqrt(power);
    for (double& a : *amp) {
      a *= scale;
    }
  }
  for (auto* table : {&tables.pure, &tables.pure_high, &tables.pure_other, &tables.buzz,
                      &tables.buzz_high, &tables.buzz_other}) {
    table->assign(hum_table_size + 1, 0.0);
  }
  Rng phases;
  phases.seed(hum_phase_seed);
  for (std::size_t n = 1; n <= hum_harmonics; ++n) {
    const double other = two_pi * phases.unit();
    // Pure is in sine phase, buzz in cosine phase: their harmonics are
    // orthogonal, so a crossfade's power is the sum of theirs.
    const double pure = tables.pure_amp[n - 1];
    const double buzz = tables.buzz_amp[n - 1];
    if (pure > 0.0) {
      add_harmonic(tables.pure, n, pure, 0.0);
    }
    add_harmonic(tables.buzz, n, buzz, quarter_turn);
    if (n >= hum_wide_from) {
      if (pure > 0.0) {
        add_harmonic(tables.pure_high, n, pure, 0.0);
        add_harmonic(tables.pure_other, n, pure, other);
      }
      add_harmonic(tables.buzz_high, n, buzz, quarter_turn);
      add_harmonic(tables.buzz_other, n, buzz, other);
    }
  }
  for (auto* table : {&tables.pure, &tables.pure_high, &tables.pure_other, &tables.buzz,
                      &tables.buzz_high, &tables.buzz_other}) {
    (*table)[hum_table_size] = (*table)[0];
  }
  return tables;
}

} // namespace

const HumTables& hum_tables() {
  static const HumTables tables = build_hum_tables();
  return tables;
}

void Hum::prepare(LayerFormat format) {
  const double rate = format.rate;
  const double reference = format.reference;
  rate_ = rate;
  reference_ = reference;
  tables_ = &hum_tables();
  for (auto& f : fizz_hp_) {
    f.set(hum_fizz_hp_hz, rate);
  }
}

void Hum::reset() noexcept {
  for (std::size_t c = 0; c < channels; ++c) {
    low_[c].reset();
    fizz_hp_[c].reset();
  }
  waver_.reset();
}

void Hum::set(std::span<const double> v, LayerShape shape) noexcept {
  const double tone = shape.tone;
  const double width = shape.width;
  const auto mains = choice_of(v[p::hum_mains], mains_hz.size());
  hz_ = mains_hz[mains];
  const double buzz = v[p::hum_buzz];
  pure_ = std::cos(quarter_turn * buzz);
  buzz_ = std::sin(quarter_turn * buzz);
  fizz_ = hum_fizz * buzz * buzz;
  const double theta = quarter_turn * std::clamp(width, 0.0, 1.0);
  keep_ = 1.0 - std::cos(theta);
  other_ = std::sin(theta);
  if (tone != tone_ || buzz != buzz_set_ || hz_ != hz_set_) {
    tone_ = tone;
    buzz_set_ = buzz;
    hz_set_ = hz_;
    const double corner = hum_tone_low_hz * std::pow(hum_tone_ratio, tone);
    for (auto& f : low_) {
      f.set(corner, butterworth_q, rate_);
    }
    // The tonal power through the low-pass, harmonic by harmonic.
    const Stage stage = low2(corner, butterworth_q);
    const double g = warp(corner, rate_);
    double power = 0.0;
    for (std::size_t n = 1; n <= hum_harmonics; ++n) {
      const double f = hz_ * static_cast<double>(n);
      if (f >= nyquist_guard * rate_) {
        break;
      }
      const double h = stage_power(stage, std::tan(pi * f / rate_) / g);
      const double pa = tables_->pure_amp[n - 1];
      const double ba = tables_->buzz_amp[n - 1];
      power += sine_power * h * (pure_ * pure_ * pa * pa + buzz_ * buzz_ * ba * ba);
    }
    // The fizz, broadband, roughly through the low-pass too.
    const std::array fizz_chain{high1(hum_fizz_hp_hz), low2(corner, butterworth_q)};
    const double fizz = fizz_ * fizz_ * hum_fizz_gate_power * noise_power(fizz_chain, rate_);
    gain_ = reference_ / std::sqrt(std::max(power + fizz, power_floor));
  }
}

} // namespace oscillate::native_dsp::effects::dust::detail

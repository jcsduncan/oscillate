#pragma once

#include "effects/dust_table.hpp"
#include "effects/effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/dust.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

// Dust's shared parts (ADR-0069): parameter indices, the seeded generator,
// the TPT filters, the rotating sine, the smooth random wander, the noise
// power of a filter cascade (which calibrates every stationary layer), and
// width and pan placement. Every layer (vinyl.hpp .. beds.hpp), the input's
// age (age.hpp), the duck (duck.hpp) and the kernel (dust.cpp) build on them.
namespace oscillate::native_dsp::effects::dust::detail {

namespace t = dust_table;
using t::index_of;

namespace p {
inline constexpr std::size_t vinyl_crackle = index_of("dust.vinyl.crackle");
inline constexpr std::size_t vinyl_dust = index_of("dust.vinyl.dust");
inline constexpr std::size_t vinyl_wear = index_of("dust.vinyl.wear");
inline constexpr std::size_t vinyl_rumble = index_of("dust.vinyl.rumble");
inline constexpr std::size_t vinyl_speed = index_of("dust.vinyl.speed");
inline constexpr std::size_t tape_hiss = index_of("dust.tape.hiss");
inline constexpr std::size_t tape_dropouts = index_of("dust.tape.dropouts");
inline constexpr std::size_t hum_mains = index_of("dust.hum.mains");
inline constexpr std::size_t hum_buzz = index_of("dust.hum.buzz");
inline constexpr std::size_t radio_tuning = index_of("dust.radio.tuning");
inline constexpr std::size_t radio_gate = index_of("dust.radio.gate");
inline constexpr std::size_t rain_intensity = index_of("dust.rain.intensity");
inline constexpr std::size_t rain_surface = index_of("dust.rain.surface");
inline constexpr std::size_t rain_distance = index_of("dust.rain.distance");
inline constexpr std::size_t rain_tone = index_of("dust.rain.tone");
inline constexpr std::size_t wind_strength = index_of("dust.wind.strength");
inline constexpr std::size_t wind_gusts = index_of("dust.wind.gusts");
inline constexpr std::size_t room_size = index_of("dust.room.size");
inline constexpr std::size_t machine_type = index_of("dust.machine.type");
inline constexpr std::size_t machine_speed = index_of("dust.machine.speed");
inline constexpr std::size_t age_amount = index_of("dust.age.amount");
inline constexpr std::size_t age_wow = index_of("dust.age.wow");
inline constexpr std::size_t age_flutter = index_of("dust.age.flutter");
inline constexpr std::size_t age_mono = index_of("dust.age.mono");
inline constexpr std::size_t duck_amount = index_of("dust.duck.amount");
inline constexpr std::size_t duck_release = index_of("dust.duck.release");
inline constexpr std::size_t duck_source = index_of("dust.duck.source");
inline constexpr std::size_t mix_input = index_of("dust.mix.input");
inline constexpr std::size_t mix_bed = index_of("dust.mix.bed");
inline constexpr std::size_t mix_seed = index_of("dust.mix.seed");
inline constexpr std::size_t mix_output = index_of("dust.mix.output");
} // namespace p

constexpr double def(std::size_t index) { return t::parameters[index].default_value; }

inline constexpr double pi = std::numbers::pi;
inline constexpr double two_pi = 2.0 * pi;
inline constexpr double ln10 = std::numbers::ln10;
using native_dsp::dsp::butterworth_q;
// Filters keep below this share of the rate.
using native_dsp::dsp::nyquist_guard;

// Every part starts at this rate until it is prepared.
inline constexpr double nominal_rate = 48'000.0;
// Both channels of a stereo pair.
inline constexpr std::size_t channels = 2;
// Added under a level before its logarithm: Dust's meters and sidechain
// read 20 log10(x + log_floor) (kept apart from dsp::gain_to_db, whose
// clamped form reads differently at silence).
inline constexpr double log_floor = 1e-30;
// Decibels in a decade of amplitude.
inline constexpr double db_per_decade = 20.0;
// A calibration's power never divides by less than this.
inline constexpr double power_floor = 1e-12;
// A decaying event below this level is finished.
inline constexpr double silent_level = 1e-9;
// A unit sine's mean power.
inline constexpr double sine_power = 0.5;
// The middle of a 0 .. 1 control (Tone's neutral colour).
inline constexpr double control_centre = 0.5;
// Sines and cosines turned a quarter cycle (equal-power crossfades, the
// width's +-45 degrees at a quarter of that).
inline constexpr double quarter_turn = 0.5 * pi;

// A voice rings until its decay has fallen this many time constants.
inline constexpr double voice_life_tau = 7.0;
// Event layers are calibrated over this many draws of their generator.
inline constexpr std::size_t calibration_draws = 4'096;
inline constexpr std::uint64_t calibration_seed = 0x5EEDCA11B7A7E5ULL;

// ---- Small DSP ------------------------------------------------------------------------------

struct Pair {
  double left = 0.0;
  double right = 0.0;
};

using native_dsp::dsp::splitmix64;
using native_dsp::dsp::unit_from_bits;
using native_dsp::dsp::unit_scale;

// xorshift64*: fast, deterministic, allocation-free.
class Rng {
public:
  void seed(std::uint64_t key) noexcept {
    generator_ = native_dsp::dsp::Xorshift64Star::from_state(splitmix64(key) | 1U);
  }
  [[nodiscard]] std::uint64_t next() noexcept { return generator_.next(); }
  // [0, 1)
  [[nodiscard]] double unit() noexcept { return unit_from_bits(next()); }
  // (0, 1): safe for logarithms and negative powers.
  [[nodiscard]] double open_unit() noexcept {
    return (static_cast<double>(next() >> native_dsp::dsp::unit_shift) + 0.5) * unit_scale;
  }
  [[nodiscard]] double bipolar() noexcept { return 2.0 * unit() - 1.0; }
  // Unit variance.
  [[nodiscard]] double white() noexcept { return std::numbers::sqrt3 * bipolar(); }
  [[nodiscard]] double sign() noexcept { return (next() >> sign_bit) != 0U ? -1.0 : 1.0; }

private:
  static constexpr unsigned sign_bit = 63U;
  native_dsp::dsp::Xorshift64Star generator_;
};

// The lowest corner a filter is set to.
inline constexpr double lowest_corner_hz = 1.0;

[[nodiscard]] inline double warp(double hz, double rate) noexcept {
  return std::tan(pi * std::clamp(hz, lowest_corner_hz, nyquist_guard * rate) / rate);
}

// The TPT state-variable filter (Zavalishin): exact bilinear analogue of
// 1 / (s^2 + s/Q + 1), stable under modulation.
struct Svf {
  synth::SvfCoefficients c = synth::SvfCoefficients::make(0.0, 1.0 / butterworth_q);
  synth::BasicZdfSvf<double> state;

  void set(double hz, double q, double rate) noexcept {
    c = synth::SvfCoefficients::make(warp(hz, rate), 1.0 / q);
  }
  [[nodiscard]] double low(double x) noexcept { return state.process(x, c).low; }
  // Unity gain at the centre.
  [[nodiscard]] double band(double x) noexcept { return c.k * state.process(x, c).band; }
  [[nodiscard]] double high(double x) noexcept { return state.process(x, c).high; }
  // Gain `gain` at the centre, unity far from it.
  [[nodiscard]] double peak(double x, double gain) noexcept {
    return x + (gain - 1.0) * c.k * state.process(x, c).band;
  }
  void reset() noexcept { state.reset(); }
};

// The TPT one-pole: exact bilinear analogue of 1 / (s + 1).
struct OnePole {
  double gain = 0.0;
  double state = 0.0;
  void set(double hz, double rate) noexcept {
    const double g = warp(hz, rate);
    gain = g / (1.0 + g);
  }
  [[nodiscard]] double low(double x) noexcept {
    const double v = (x - state) * gain;
    const double y = v + state;
    state = y + v;
    return y;
  }
  [[nodiscard]] double high(double x) noexcept { return x - low(x); }
  void reset() noexcept { state = 0.0; }
};

// A sine by rotation: one complex multiply a frame. Frequencies are set and
// the magnitude renormalised at control ticks.
struct Osc {
  double re = 1.0;
  double im = 0.0;
  double cr = 1.0;
  double ci = 0.0;
  void set(double hz, double rate) noexcept {
    const double w = two_pi * std::clamp(hz, 0.0, nyquist_guard * rate) / rate;
    cr = std::cos(w);
    ci = std::sin(w);
  }
  [[nodiscard]] double next() noexcept {
    const double r = re * cr - im * ci;
    im = re * ci + im * cr;
    re = r;
    return im;
  }
  // One Newton step towards unit magnitude: m = (3 - |z|^2) / 2.
  void normalise() noexcept {
    const double m = 1.5 - 0.5 * (re * re + im * im);
    re *= m;
    im *= m;
  }
  void reset() noexcept {
    re = 1.0;
    im = 0.0;
  }
};

// A smooth random wander in [-1, 1]: a new target about every `seconds`
// (give or take drift_jitter), eased between with a smoothstep, so its
// slope stays bounded.
inline constexpr double drift_jitter_low = 0.7;
inline constexpr double drift_jitter_span = 0.6;

class Drift {
public:
  [[nodiscard]] double next(Rng& rng, double seconds, double rate) noexcept {
    position_ += step_;
    if (step_ == 0.0 || position_ >= 1.0) {
      from_ = to_;
      to_ = rng.bipolar();
      position_ = 0.0;
      const double length =
          std::max(1.0, seconds * rate * (drift_jitter_low + drift_jitter_span * rng.unit()));
      step_ = 1.0 / length;
    }
    // A smoothstep, 3x^2 - 2x^3, between the targets.
    value_ = from_ + (to_ - from_) * position_ * position_ * (3.0 - 2.0 * position_);
    return value_;
  }
  [[nodiscard]] double value() const noexcept { return value_; }
  void reset() noexcept {
    from_ = 0.0;
    to_ = 0.0;
    position_ = 0.0;
    step_ = 0.0;
    value_ = 0.0;
  }

private:
  double from_ = 0.0;
  double to_ = 0.0;
  double position_ = 0.0;
  double step_ = 0.0;
  double value_ = 0.0;
};

// ---- Noise power ----------------------------------------------------------------------------

// A cascade's stages, as their analogue prototypes. Unit-variance white
// noise through them has the variance noise_power returns.
enum class Kind : std::uint8_t { low1, high1, low2, high2, band2, peak2 };
inline constexpr double default_stage_hz = 1'000.0;
struct Stage {
  Kind kind = Kind::low1;
  double hz = default_stage_hz;
  double q = butterworth_q;
  double gain = 1.0;
};
constexpr Stage low1(double hz) { return {Kind::low1, hz, butterworth_q, 1.0}; }
constexpr Stage high1(double hz) { return {Kind::high1, hz, butterworth_q, 1.0}; }
constexpr Stage low2(double hz, double q) { return {Kind::low2, hz, q, 1.0}; }
constexpr Stage high2(double hz, double q) { return {Kind::high2, hz, q, 1.0}; }
constexpr Stage band2(double hz, double q) { return {Kind::band2, hz, q, 1.0}; }
constexpr Stage peak2(double hz, double q, double gain) { return {Kind::peak2, hz, q, gain}; }

// |H|^2 of one prototype stage at normalised frequency x = tan(w/2) / g.
[[nodiscard]] double stage_power(const Stage& stage, double x) noexcept;

// The mean of |H(w)|^2 over [0, pi]: integrated on a logarithmic grid so
// low corners and narrow resonances are resolved as well as broad bands.
inline constexpr std::size_t max_stages = 6;
[[nodiscard]] double noise_power(std::span<const Stage> stages, double rate) noexcept;

// A narrow band-pass of unity peak: its share of white noise's power.
[[nodiscard]] inline double band_power(double hz, double q, double rate) noexcept {
  return pi * hz / (q * rate);
}

// E[a^2] of a Pareto size (minimum 1, tail alpha) clamped at `cap`.
[[nodiscard]] double pareto_square_mean(double alpha, double cap) noexcept;

// The layer's width: two decorrelated signals of equal spectrum placed at
// +-45 degrees at full width, the same in both channels at none. Power is
// kept at every width.
struct Width {
  double c = 1.0;
  double s = 0.0;
  void set(double width) noexcept {
    const double theta = 0.5 * quarter_turn * std::clamp(width, 0.0, 1.0);
    c = std::cos(theta);
    s = std::sin(theta);
  }
  [[nodiscard]] Pair place(double a, double b) const noexcept {
    return {a * c + b * s, a * c - b * s};
  }
};

// Equal-power placement of a mono event: each channel keeps its mean power.
[[nodiscard]] inline Pair pan_gains(double pan) noexcept {
  const double x = std::clamp(pan, -1.0, 1.0);
  return {std::sqrt(1.0 - x), std::sqrt(1.0 + x)};
}

[[nodiscard]] inline double gain_of(double db) noexcept { return dsp::db_to_gain(db); }

// A choice parameter's index, held inside a table of `size` entries.
[[nodiscard]] inline std::size_t choice_of(double value, std::size_t size) noexcept {
  return std::min(static_cast<std::size_t>(value), size - 1);
}

} // namespace oscillate::native_dsp::effects::dust::detail

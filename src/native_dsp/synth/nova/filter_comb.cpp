#include "synth/nova/filter_comb.hpp"

#include "synth/nova/filter_impl.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace oscillate::native_dsp::synth::nova {
namespace {

// The highest pitch a comb (or flanger) is tuned to: the rate over this.
constexpr double comb_top_share = 2.5;
constexpr double flanger_top_share = 4.0;
// The loop's damping falls from comb_damping_max_hz by comb_damping_ratio with FAT.
constexpr double comb_damping_max_hz = 20'000.0;
constexpr double comb_damping_ratio = 0.03;
// The Lagrange read needs this much delay at least.
constexpr double min_delay_frames = 2.0;
// The loops' feedback at full resonance.
constexpr double comb_max_feedback = 0.99;
constexpr double flanger_max_feedback = 0.85;

// Phase delay, in frames, of a trapezoidal one-pole low-pass (gain g) at
// w radians per frame: the comb loops subtract it so they stay in tune.
[[nodiscard]] double one_pole_lag(double g, double w) noexcept {
  return std::atan(std::tan(0.5 * w) / g) / w;
}

} // namespace

void VoiceFilter::Impl::derive_comb(FilterModel model, const Params& p, double rate,
                                    Coefficients& c) noexcept {
  using namespace filter_detail;
  const double r = p.resonance;
  const double fat = p.fat;
  const double fc = p.cutoff_hz;
  const bool flanger = model == FilterModel::flanger;
  c.sign = model == FilterModel::comb_minus ? -1.0 : 1.0;
  const double hz =
      std::clamp(fc, lowest_delay_hz, rate / (flanger ? flanger_top_share : comb_top_share));
  const double damping_hz = comb_damping_max_hz * std::pow(comb_damping_ratio, fat);
  c.g = warp(damping_hz, rate);
  c.damping = c.g / (1.0 + c.g);
  // Comb +: peaks on every multiple of the cutoff. Comb −: notches
  // there, peaks halfway (an octave down). Flanger: the first notch sits
  // on the cutoff (half-period delay). The damping filter's lag at that
  // frequency is taken out of the loop so all three stay in tune.
  double tuned_hz = hz;
  double period = rate / hz;
  if (model == FilterModel::comb_minus) {
    tuned_hz = 0.5 * hz;
  } else if (flanger) {
    period = 0.5 * rate / hz;
  }
  const double w = two_pi * tuned_hz / rate;
  c.delay = std::clamp(period - one_pole_lag(c.g, w), min_delay_frames, rate / lowest_delay_hz);
  c.k = flanger ? flanger_max_feedback * r : comb_max_feedback * (1.0 - (1.0 - r) * (1.0 - r));
  // Broadband (noise) power through the comb is 1 + 1 / (1 - k^2).
  c.output_gain = 1.0 / std::sqrt(1.0 + 1.0 / (1.0 - c.k * c.k));
}

VoiceFilter::Impl::Complex VoiceFilter::Impl::response_comb(const Coefficients& c,
                                                            const ResponsePoint& at) noexcept {
  const Complex loop = std::polar(1.0, -at.w * c.delay) / (1.0 + at.s_of(c.g));
  return c.output_gain * (1.0 + c.sign * loop / (1.0 - c.sign * c.k * loop));
}

} // namespace oscillate::native_dsp::synth::nova

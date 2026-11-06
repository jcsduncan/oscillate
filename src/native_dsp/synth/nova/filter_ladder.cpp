#include "synth/nova/filter_ladder.hpp"

#include "synth/nova/filter_impl.hpp"

#include <cstddef>

namespace oscillate::native_dsp::synth::nova {

void VoiceFilter::Impl::derive_ladder(FilterModel model, const Params& p, double rate,
                                      Coefficients& c) noexcept {
  using namespace filter_detail;
  const double r = p.resonance;
  const double fat = p.fat;
  const double fc = p.cutoff_hz;
  const Voicing& v = voicing(model);
  c.g = warp(fc, rate);
  if (model == FilterModel::diode) {
    c.g /= diode_tuning(v.max_feedback * r);
  }
  c.G = c.g / (1.0 + c.g);
  c.k = v.max_feedback * r;
  const double G2 = c.G * c.G;
  c.solve = 1.0 / (1.0 + c.k * G2 * G2);
  c.input_gain = (1.0 + (v.compensation + (1.0 - v.compensation) * fat) * c.k) / v.headroom;
  const double share = c.k / v.max_feedback;
  c.output_gain = v.headroom * (1.0 + (v.resonance_gain - 1.0) * share * share);
}

VoiceFilter::Impl::Complex VoiceFilter::Impl::response_ladder(FilterModel model,
                                                              const Coefficients& c,
                                                              const ResponsePoint& at) noexcept {
  if (model != FilterModel::diode) {
    const Complex G = 1.0 / (1.0 + at.s_of(c.g));
    const Complex G2 = G * G;
    const Complex tap = model == FilterModel::ladder12 ? G2 : G2 * G2;
    return c.input_gain * tap / (1.0 + c.k * G2 * G2) * c.output_gain;
  }
  // (s I - A) y = e1 u, by the same tridiagonal elimination.
  const Complex s = at.s_of(c.g);
  const Complex d1 = s + 2.0;
  const Complex d2 = s + 2.0;
  const Complex d3 = s + 2.0;
  const Complex d4 = s + 2.0;
  const Complex cp1 = -1.0 / d1;
  const Complex q1 = 1.0 / d1;
  const Complex e2 = 1.0 / (d2 + cp1);
  const Complex cp2 = -e2;
  const Complex q2 = q1 * e2;
  const Complex e3 = 1.0 / (d3 + cp2);
  const Complex cp3 = -e3;
  const Complex q3 = q2 * e3;
  const Complex e4 = 1.0 / (d4 + 2.0 * cp3);
  const Complex h = 2.0 * q3 * e4;
  return c.input_gain * h / (1.0 + c.k * h) * c.output_gain;
}

} // namespace oscillate::native_dsp::synth::nova

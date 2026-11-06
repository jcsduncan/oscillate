#pragma once

#include "synth/nova/filter_impl.hpp"

#include <cstddef>

// Nova's character family: Formant and Vowel (banks of band-passes and
// bells singing A-E-I-O-U), Phaser, the LP→BP→HP morph, Dual Peak and
// Ring. Coefficients and responses are in filter_character.cpp.
namespace oscillate::native_dsp::synth::nova {

inline double VoiceFilter::Impl::character(FilterModel model, const Coefficients& c, Memory& m,
                                           double x) noexcept {
  using namespace filter_detail;
  switch (model) {
  case FilterModel::formant: {
    double sum = 0.0;
    for (std::size_t f = 0; f < formant_bands; ++f) {
      const auto& band = c.band[f];
      sum += band.gain * svf(x, band.g, band.k, m[2 * f], m[2 * f + 1]).band;
    }
    return sum * c.output_gain;
  }
  case FilterModel::vowel: {
    double y = x;
    for (std::size_t f = 0; f < vowel_bands; ++f) {
      const auto& band = c.band[f];
      y += band.gain * svf(y, band.g, band.k, m[2 * f], m[2 * f + 1]).band;
    }
    return y * c.output_gain;
  }
  case FilterModel::phaser: {
    // The all-pass chain is affine in its input, y = A u + B, so the
    // feedback resolves without a delay; |k A| < 1 keeps it stable.
    double a_total = 1.0;
    double b_total = 0.0;
    for (std::size_t i = 0; i < phaser_stages; ++i) {
      const double G = c.band[i].k;
      const double a = 2.0 * G - 1.0;
      a_total *= a;
      b_total = a * b_total + 2.0 * (1.0 - G) * m[i];
    }
    double u = (x + c.k * b_total) / (1.0 - c.k * a_total);
    u = phaser_headroom * soft_clip(u / phaser_headroom);
    for (std::size_t i = 0; i < phaser_stages; ++i) {
      u = 2.0 * one_pole(u, c.band[i].k, m[i]) - u;
    }
    return 0.5 * (x + u);
  }
  case FilterModel::morph_lbh: {
    const auto mix = [&](const SvfOut& o, double k) {
      return c.weight_low * o.low + c.weight_band * k * o.band + c.weight_high * o.high;
    };
    const double first = mix(svf(x, c.g, c.k, m[0], m[1]), c.k);
    // Only the low and high shares carry a resonant boost to limit.
    const double k2 = c.k2 + svf_limit * (c.weight_low + c.weight_high) * m[2] * m[2];
    return mix(svf(first, c.g, k2, m[2], m[3]), c.k2);
  }
  case FilterModel::dual_peak: {
    const double low = svf(x, c.band[0].g, c.k, m[0], m[1]).band;
    const double high = svf(x, c.band[1].g, c.k, m[2], m[3]).band;
    return c.k * (low + high);
  }
  case FilterModel::ring: {
    const double sine = sin_cycle(m[0]);
    m[0] += c.g;
    m[0] -= fast_floor(m[0]);
    const double carrier = sine + c.stage_mix * (soft_clip(c.stage_drive * sine) - sine);
    return x + c.k * (x * carrier * c.output_gain - x);
  }
  default:
    return x;
  }
}

// svf() for both channels at once (see Pair), returning the band output.
// The coefficients are the same for both, so they are derived once; every
// other operation is the scalar one, in its order, on each lane.
inline VoiceFilter::Impl::Pair VoiceFilter::Impl::svf_band_pair(Pair x, double g, double k,
                                                                double& left1, double& right1,
                                                                double& left2,
                                                                double& right2) noexcept {
  const auto c = SvfCoefficients::make(g, k);
  const Pair ic1(left1, right1);
  const Pair ic2(left2, right2);
  const Pair v3 = x - ic2;
  const Pair v1 = c.a1 * ic1 + c.a2 * v3;
  const Pair v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
  const Pair next1 = 2.0 * v1 - ic1;
  const Pair next2 = 2.0 * v2 - ic2;
  left1 = next1.first();
  right1 = next1.second();
  left2 = next2.first();
  right2 = next2.second();
  return v1;
}

// Formant and Vowel for both channels at once: character() for each,
// lane by lane, then run()'s flush of their memory.
inline void VoiceFilter::Impl::character_pair(Slot& slot, double& left, double& right) noexcept {
  using namespace filter_detail;
  const Coefficients& c = slot.c;
  Memory& ml = slot.memory[0];
  Memory& mr = slot.memory[1];
  const Pair x(left, right);
  Pair y = x;
  if (slot.model == FilterModel::formant) {
    Pair sum(0.0, 0.0);
    for (std::size_t f = 0; f < formant_bands; ++f) {
      const auto& band = c.band[f];
      sum = sum + band.gain * svf_band_pair(x, band.g, band.k, ml[2 * f], mr[2 * f], ml[2 * f + 1],
                                            mr[2 * f + 1]);
    }
    y = sum * c.output_gain;
  } else {
    for (std::size_t f = 0; f < vowel_bands; ++f) {
      const auto& band = c.band[f];
      y = y + band.gain * svf_band_pair(y, band.g, band.k, ml[2 * f], mr[2 * f], ml[2 * f + 1],
                                        mr[2 * f + 1]);
    }
    y = y * c.output_gain;
  }
  left = y.first();
  right = y.second();
}

} // namespace oscillate::native_dsp::synth::nova

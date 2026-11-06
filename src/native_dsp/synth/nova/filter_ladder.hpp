#pragma once

#include "synth/nova/filter_impl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

// Nova's ladder family: Ladder 24 and 12, MG Low and OTA (four one-poles
// around a saturating feedback loop, voiced apart) and the coupled Diode
// ladder. Coefficients and responses are in filter_ladder.cpp.
namespace oscillate::native_dsp::synth::nova::filter_detail {

// ---- Analog voicings ------------------------------------------------------------------

struct Voicing {
  double max_feedback;   // Feedback at resonance 1 (self-oscillation above the threshold).
  double headroom;       // Input is divided by this, output multiplied.
  double stage_headroom; // Stage-input saturation scale (the diode: branch scale).
  double compensation;   // Share of the bass loss restored at FAT 0; FAT 1 restores all.
  double resonance_gain; // Output gain at full feedback (rises with feedback squared).
  bool cubic_input;
  bool cubic_stages;
};

// Ladder: a smooth rational tanh at the summing input and gentle cubic
// stages; self-oscillates at 4 (resonance ~0.91).
inline constexpr Voicing ladder_voicing{4.4, 1.5, 2.5, 0.4, 1.5, false, true};
// MG Low: a harder input knee, stages that saturate early, and the full
// bass loss of the original unless FAT restores it.
inline constexpr Voicing mg_voicing{4.5, 1.2, 1.2, 0.0, 1.8, true, false};
// OTA: every stage saturates hard and resonance keeps three quarters of its
// bass loss, so it thins and screams like the IR3109-family cascades.
inline constexpr Voicing ota_voicing{4.3, 1.0, 0.8, 0.25, 3.5, false, false};
// Diode: the TB-style coupled ladder. Its linear threshold is 17, where it
// rings at sqrt 2 times the stage corner; with no feedback its -3 dB point
// is 0.149 times the corner (the poles spread widely, hence the soft knee).
inline constexpr Voicing diode_voicing{18.0, 6.0, diode_branch_headroom, 0.5, 2.0, false, false};
inline constexpr double diode_threshold = 17.0;
inline constexpr double diode_corner_ratio = 0.14935631;

// Cutoff over the diode's stage corner: the -3 dB point with no feedback,
// moving (as sqrt of the feedback share, which tracks the resonant peak
// within a few percent) to the oscillation frequency from the threshold up,
// so self-oscillation is exactly on the cutoff.
[[nodiscard]] inline double diode_tuning(double feedback) noexcept {
  const double share = std::sqrt(std::clamp(feedback / diode_threshold, 0.0, 1.0));
  return diode_corner_ratio + (std::numbers::sqrt2 - diode_corner_ratio) * share;
}

[[nodiscard]] inline const Voicing& voicing(FilterModel model) noexcept {
  switch (model) {
  case FilterModel::mg_low:
    return mg_voicing;
  case FilterModel::ota:
    return ota_voicing;
  case FilterModel::diode:
    return diode_voicing;
  default:
    return ladder_voicing;
  }
}

// A diode branch this close to zero volts conducts as a plain resistor.
inline constexpr double diode_linear_volts = 1e-9;

} // namespace oscillate::native_dsp::synth::nova::filter_detail

namespace oscillate::native_dsp::synth::nova {

inline double VoiceFilter::Impl::ladder(FilterModel model, const Coefficients& c, Memory& m,
                                        std::uint32_t& noise, double x) noexcept {
  using namespace filter_detail;
  const Voicing& v = voicing(model);
  const double G = c.G;
  const double in = x * c.input_gain + noise_level * white(noise);
  const double tail = (1.0 - G) * (G * G * G * m[0] + G * G * m[1] + G * m[2] + m[3]);
  const double sum = (in - c.k * tail) * c.solve;
  const double u = v.cubic_input ? soft_clip_cubic(sum) : soft_clip(sum);
  const double h = v.stage_headroom;
  const auto stage_input = [&](double y) {
    return h * (v.cubic_stages ? soft_clip_cubic(y / h) : soft_clip(y / h));
  };
  const double y1 = one_pole(u, G, m[0]);
  const double y2 = one_pole(stage_input(y1), G, m[1]);
  const double y3 = one_pole(stage_input(y2), G, m[2]);
  const double y4 = one_pole(stage_input(y3), G, m[3]);
  return (model == FilterModel::ladder12 ? y2 : y4) * c.output_gain;
}

// ladder() for both channels at once (see Pair): the same operations in
// the same order as two scalar calls.
inline void VoiceFilter::Impl::ladder_pair(FilterModel model, Slot& slot, double& left,
                                           double& right) noexcept {
  using namespace filter_detail;
  const Voicing& v = voicing(model);
  const Coefficients& c = slot.c;
  Memory& ml = slot.memory[0];
  Memory& mr = slot.memory[1];
  const double G = c.G;
  const double noise_left = noise_level * white(slot.noise[0]);
  const double noise_right = noise_level * white(slot.noise[1]);
  const Pair in = Pair(left, right) * c.input_gain + Pair(noise_left, noise_right);
  std::array<Pair, 4> m{Pair(ml[0], mr[0]), Pair(ml[1], mr[1]), Pair(ml[2], mr[2]),
                        Pair(ml[3], mr[3])};
  const Pair tail = (1.0 - G) * (G * G * G * m[0] + G * G * m[1] + G * m[2] + m[3]);
  const Pair sum = (in - c.k * tail) * c.solve;
  const Pair u = v.cubic_input ? soft_clip_cubic(sum) : soft_clip(sum);
  const double h = v.stage_headroom;
  const auto stage_input = [&](Pair y) {
    return h * (v.cubic_stages ? soft_clip_cubic(y / h) : soft_clip(y / h));
  };
  const auto pole = [G](Pair x, Pair& state) {
    const Pair d = (x - state) * G;
    const Pair y = d + state;
    state = y + d;
    return y;
  };
  const Pair y1 = pole(u, m[0]);
  const Pair y2 = pole(stage_input(y1), m[1]);
  const Pair y3 = pole(stage_input(y2), m[2]);
  const Pair y4 = pole(stage_input(y3), m[3]);
  const Pair out = (model == FilterModel::ladder12 ? y2 : y4) * c.output_gain;
  for (std::size_t i = 0; i < m.size(); ++i) {
    ml[i] = m[i].first();
    mr[i] = m[i].second();
  }
  left = out.first();
  right = out.second();
}

// The diode ladder: four capacitors coupled through diode pairs (the last
// at half capacitance). Each branch's conductance follows its previous
// voltage, tanh(v)/v, and the linearised coupled system is solved exactly
// (a tridiagonal solve with the feedback resolved). Memory: states 0..3,
// outputs 4..7, the input stage 8.
inline double VoiceFilter::Impl::diode(const Coefficients& c, Memory& m, std::uint32_t& noise,
                                       double x) noexcept {
  using namespace filter_detail;
  const auto conductance = [](double voltage) {
    const double t = voltage / diode_branch_headroom;
    return std::fabs(t) < diode_linear_volts ? 1.0 : soft_clip(t) / t;
  };
  const double g = c.g;
  const double t1 = conductance(m[8] - m[4]);
  const double t2 = conductance(m[4] - m[5]);
  const double t3 = conductance(m[5] - m[6]);
  const double t4 = conductance(m[6] - m[7]);
  // (I - g A) y = s + g t1 e1 u, eliminated forward for the states (p)
  // and the input (q) at once.
  const double d1 = 1.0 + g * (t1 + t2);
  const double c1 = -g * t2;
  const double l2 = -g * t2;
  const double d2 = 1.0 + g * (t2 + t3);
  const double c2 = -g * t3;
  const double l3 = -g * t3;
  const double d3 = 1.0 + g * (t3 + t4);
  const double c3 = -g * t4;
  const double l4 = -2.0 * g * t4;
  const double d4 = 1.0 + 2.0 * g * t4;
  const double e1 = 1.0 / d1;
  const double cp1 = c1 * e1;
  const double p1 = m[0] * e1;
  const double q1 = g * t1 * e1;
  const double e2 = 1.0 / (d2 - l2 * cp1);
  const double cp2 = c2 * e2;
  const double p2 = (m[1] - l2 * p1) * e2;
  const double q2 = -l2 * q1 * e2;
  const double e3 = 1.0 / (d3 - l3 * cp2);
  const double cp3 = c3 * e3;
  const double p3 = (m[2] - l3 * p2) * e3;
  const double q3 = -l3 * q2 * e3;
  const double e4 = 1.0 / (d4 - l4 * cp3);
  const double p4 = (m[3] - l4 * p3) * e4;
  const double q4 = -l4 * q3 * e4;
  const double in = x * c.input_gain + noise_level * white(noise);
  const double u = soft_clip((in - c.k * p4) / (1.0 + c.k * q4));
  const double y4 = p4 + q4 * u;
  const double y3 = p3 + q3 * u - cp3 * y4;
  const double y2 = p2 + q2 * u - cp2 * y3;
  const double y1 = p1 + q1 * u - cp1 * y2;
  m[0] = 2.0 * y1 - m[0];
  m[1] = 2.0 * y2 - m[1];
  m[2] = 2.0 * y3 - m[2];
  m[3] = 2.0 * y4 - m[3];
  m[4] = y1;
  m[5] = y2;
  m[6] = y3;
  m[7] = y4;
  m[8] = u;
  return y4 * c.output_gain;
}

} // namespace oscillate::native_dsp::synth::nova

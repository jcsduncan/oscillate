#include "synth/nova/filter_svf.hpp"

#include "synth/nova/filter_impl.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oscillate::native_dsp::synth::nova {
namespace {

using namespace filter_detail;

// Notch: Q from notch_q_min (wide) to notch_q_min * notch_q_ratio (surgical).
constexpr double notch_q_min = 0.5;
constexpr double notch_q_ratio = 40.0;
// Peak: a constant-Q bell resonance raises from peak_min_db by up to
// peak_span_db while its Q rises from peak_q_min by up to peak_q_span.
constexpr double peak_min_db = 3.0;
constexpr double peak_span_db = 15.0;
constexpr double peak_q_min = 0.5;
constexpr double peak_q_span = 4.5;
// FAT's second stage: fully in by 1 / second_stage_fade of the knob, driven
// by up to second_stage_max_db, with a makeup that keeps its loudness.
constexpr double second_stage_fade = 10.0;
constexpr double second_stage_max_db = 18.0;
constexpr double second_stage_makeup_law = 0.3;

void set_second_stage(double fat, double& drive, double& makeup, double& mix) noexcept {
  mix = std::min(1.0, second_stage_fade * fat);
  drive = db_to_gain(second_stage_max_db * fat);
  makeup = (1.0 + second_stage_makeup_law * std::log(drive)) / drive;
}

} // namespace

void VoiceFilter::Impl::derive_svf(FilterModel model, const Params& p, double rate,
                                   Coefficients& c) noexcept {
  using namespace filter_detail;
  const double r = p.resonance;
  const double fat = p.fat;
  const double fc = p.cutoff_hz;
  switch (model) {
  case FilterModel::lp12:
  case FilterModel::bp12:
  case FilterModel::hp12:
    c.g = warp(fc, rate);
    c.k = 1.0 / resonance_q(r, oscillate::native_dsp::dsp::butterworth_q);
    set_second_stage(fat, c.stage_drive, c.stage_makeup, c.stage_mix);
    return;
  case FilterModel::lp24:
  case FilterModel::hp24:
    c.g = warp(fc, rate);
    c.k = butterworth_k1;
    c.k2 = 1.0 / resonance_q(r, butterworth_q2);
    set_second_stage(fat, c.stage_drive, c.stage_makeup, c.stage_mix);
    return;
  case FilterModel::bp24:
    c.g = warp(fc, rate);
    c.k = std::numbers::sqrt2;
    c.k2 = 1.0 / resonance_q(r, oscillate::native_dsp::dsp::butterworth_q);
    set_second_stage(fat, c.stage_drive, c.stage_makeup, c.stage_mix);
    return;
  case FilterModel::notch:
    c.g = warp(fc, rate);
    c.k = 1.0 / (notch_q_min * std::pow(notch_q_ratio, r));
    set_second_stage(fat, c.stage_drive, c.stage_makeup, c.stage_mix);
    return;
  case FilterModel::peak: {
    // A constant-Q bell: resonance raises it from +3 to +18 dB and narrows it.
    const double boost = std::pow(10.0, (peak_min_db + peak_span_db * r) / bell_db_per_decade);
    const double q = peak_q_min + peak_q_span * r;
    c.g = warp(fc, rate);
    c.k = 1.0 / (q * boost);
    c.k2 = c.k * (boost * boost - 1.0);
    set_second_stage(fat, c.stage_drive, c.stage_makeup, c.stage_mix);
    return;
  }
  default:
    return;
  }
}

VoiceFilter::Impl::Complex VoiceFilter::Impl::response_svf(FilterModel model, const Coefficients& c,
                                                           const ResponsePoint& at) noexcept {
  const double stage = 1.0 - c.stage_mix + c.stage_mix * c.stage_drive * c.stage_makeup;
  switch (model) {
  case FilterModel::lp12:
    return at.svf_at(c.g, c.k).low * stage;
  case FilterModel::bp12:
    return c.k * at.svf_at(c.g, c.k).band * stage;
  case FilterModel::hp12:
    return at.svf_at(c.g, c.k).high * stage;
  case FilterModel::lp24:
    return at.svf_at(c.g, c.k).low * stage * at.svf_at(c.g, c.k2).low;
  case FilterModel::bp24:
    return c.k * at.svf_at(c.g, c.k).band * stage * c.k2 * at.svf_at(c.g, c.k2).band;
  case FilterModel::hp24:
    return at.svf_at(c.g, c.k).high * stage * at.svf_at(c.g, c.k2).high;
  case FilterModel::notch: {
    const ResponsePoint::Sections o = at.svf_at(c.g, c.k);
    return (o.low + o.high) * stage;
  }
  case FilterModel::peak:
    return (1.0 + c.k2 * at.svf_at(c.g, c.k).band) * stage;
  default:
    return 1.0;
  }
}

} // namespace oscillate::native_dsp::synth::nova

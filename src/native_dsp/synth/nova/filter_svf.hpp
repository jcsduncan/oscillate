#pragma once

#include "synth/nova/filter_impl.hpp"

#include <cstddef>

// Nova's clean family: Simper SVFs as LP, BP and HP 12 and 24, Notch and
// Peak, each with FAT's saturating second stage. Coefficients and responses
// are in filter_svf.cpp.
namespace oscillate::native_dsp::synth::nova::filter_detail {

// FAT on the clean models: drive into the second stage (between the two
// sections of a 24 dB model, after the section of a 12 dB one).
[[nodiscard]] inline double second_stage(double x, double drive, double makeup,
                                         double mix) noexcept {
  if (mix <= 0.0) {
    return x;
  }
  return x + mix * (soft_clip(drive * x) * makeup - x);
}

} // namespace oscillate::native_dsp::synth::nova::filter_detail

namespace oscillate::native_dsp::synth::nova {

inline double VoiceFilter::Impl::clean(FilterModel model, const Coefficients& c, Memory& m,
                                       double x) noexcept {
  using namespace filter_detail;
  const auto stage = [&](double y) {
    return second_stage(y, c.stage_drive, c.stage_makeup, c.stage_mix);
  };
  switch (model) {
  case FilterModel::lp12:
    return stage(svf(x, c.g, limited(c.k, m[0]), m[0], m[1]).low);
  case FilterModel::bp12:
    // Normalised band-passes peak at unity, so they need no limiting.
    return stage(c.k * svf(x, c.g, c.k, m[0], m[1]).band);
  case FilterModel::hp12:
    return stage(svf(x, c.g, limited(c.k, m[0]), m[0], m[1]).high);
  case FilterModel::lp24: {
    const double first = stage(svf(x, c.g, c.k, m[0], m[1]).low);
    return svf(first, c.g, limited(c.k2, m[2]), m[2], m[3]).low;
  }
  case FilterModel::bp24: {
    const double first = stage(c.k * svf(x, c.g, c.k, m[0], m[1]).band);
    return c.k2 * svf(first, c.g, c.k2, m[2], m[3]).band;
  }
  case FilterModel::hp24: {
    const double first = stage(svf(x, c.g, c.k, m[0], m[1]).high);
    return svf(first, c.g, limited(c.k2, m[2]), m[2], m[3]).high;
  }
  case FilterModel::notch: {
    const SvfOut o = svf(x, c.g, c.k, m[0], m[1]);
    return stage(o.low + o.high);
  }
  case FilterModel::peak:
    return stage(x + c.k2 * svf(x, c.g, c.k, m[0], m[1]).band);
  default:
    return x;
  }
}

} // namespace oscillate::native_dsp::synth::nova

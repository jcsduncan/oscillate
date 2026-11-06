#include "oscillate/native_dsp/synth/oscillator.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace oscillate::native_dsp::synth {
namespace {

constexpr std::size_t integration_substeps = 16;

double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double quarter_square = x * x / 4.0;
  for (int k = 1; k < 64 && term > sum * 1e-17; ++k) {
    term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
  }
  return sum;
}

double kernel(const BlepKernel& spec, double t) {
  const double half = spec.half_width;
  const double ratio = t / half;
  if (ratio <= -1.0 || ratio >= 1.0) {
    return 0.0;
  }
  const double x = 2.0 * spec.cutoff * t;
  const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
  const double window =
      bessel_i0(spec.beta * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(spec.beta);
  return 2.0 * spec.cutoff * sinc * window;
}

} // namespace

template <BlepKernel Kernel> const BasicBlepTables<Kernel>& BasicBlepTables<Kernel>::instance() {
  static const BasicBlepTables tables;
  return tables;
}

template <BlepKernel Kernel> BasicBlepTables<Kernel>::BasicBlepTables() {
  const auto kernel = [](double t) { return synth::kernel(Kernel, t); };
  const std::size_t points = 2 * half_width * blep_table_resolution + 1;
  const double spacing = 1.0 / blep_table_resolution;
  const double substep = spacing / integration_substeps;
  step_.assign(points, 0.0);
  ramp_.assign(points, 0.0);

  // Simpson integration of the kernel between table points, then trapezoid
  // integration of that running integral for the ramp table.
  for (std::size_t point = 1; point < points; ++point) {
    const double begin =
        -static_cast<double>(half_width) + static_cast<double>(point - 1) * spacing;
    double area = 0.0;
    for (std::size_t sub = 0; sub < integration_substeps; ++sub) {
      const double a = begin + static_cast<double>(sub) * substep;
      area += substep / 6.0 * (kernel(a) + 4.0 * kernel(a + substep / 2.0) + kernel(a + substep));
    }
    step_[point] = step_[point - 1] + area;
  }
  const double total = step_.back();
  for (double& value : step_) {
    value /= total;
  }
  for (std::size_t point = 1; point < points; ++point) {
    ramp_[point] = ramp_[point - 1] + spacing * 0.5 * (step_[point - 1] + step_[point]);
  }
}

} // namespace oscillate::native_dsp::synth

namespace oscillate::native_dsp::synth {

template <BlepKernel Kernel>
float BasicBlepOscillator<Kernel>::next_general(double increment, double pulse_width,
                                                SyncPoint sync, SyncPoint* wrap) noexcept {
  if (wrap != nullptr) {
    *wrap = {};
  }
  if (pending_step_ != 0.0) {
    apply_step(pending_step_, 0.5);
    pending_step_ = 0.0;
  }

  double phase = phase_;
  const double start_phase = phase;
  bool wrapped = false;
  if (sync.active) {
    wrapped = true;
    const double until = 1.0 - sync.since;
    advance(phase, increment, pulse_width, 0.0, until, wrap);
    const double value_before = naive(phase, waveform_, pulse_width);
    const double slope_before = slope(phase, waveform_);
    phase = 0.0;
    apply_step(naive(0.0, waveform_, pulse_width) - value_before, sync.since);
    apply_ramp((slope(0.0, waveform_) - slope_before) * increment, sync.since);
    pulse_high_ = true;
    advance(phase, increment, pulse_width, until, sync.since, wrap);
  } else {
    wrapped = advance(phase, increment, pulse_width, 0.0, 1.0, wrap);
  }
  phase_ = phase;

  if (waveform_ == Waveform::pulse) {
    // A width sweep can pass the phase without the phase reaching it: find
    // where phase - width changed sign over the frame and step there.
    const bool high = phase < pulse_width;
    if (high != pulse_high_) {
      const double before = start_phase - last_width_;
      const double after = phase - pulse_width;
      double since = 0.5;
      if (!wrapped && before != after) {
        const double crossing = before / (before - after);
        since = crossing > 0.0 && crossing < 1.0 ? 1.0 - crossing : 0.5;
      }
      apply_step(high ? 2.0 : -2.0, since);
      pulse_high_ = high;
    }
    last_width_ = pulse_width;
  }
  return emit(naive(phase, waveform_, pulse_width));
}

template <BlepKernel Kernel>
double BasicBlepOscillator<Kernel>::slope(double phase, Waveform waveform) noexcept {
  switch (waveform) {
  case Waveform::saw:
    return 2.0;
  case Waveform::pulse:
    return 0.0;
  case Waveform::triangle:
    return phase < 0.5 ? 4.0 : -4.0;
  case Waveform::sine:
    return 2.0 * std::numbers::pi * sin_cycle(phase + 0.25);
  }
  return 0.0;
}

template <BlepKernel Kernel>
bool BasicBlepOscillator<Kernel>::advance(double& phase, double increment, double width,
                                          double start, double length, SyncPoint* wrap) noexcept {
  double remaining = increment * length;
  double time = start;
  bool wrapped = false;
  while (remaining > 0.0) {
    double event = 1.0;
    if (waveform_ == Waveform::pulse && pulse_high_ && phase < width) {
      event = width;
    } else if (waveform_ == Waveform::triangle && phase < 0.5) {
      event = 0.5;
    }
    const double distance = event - phase;
    if (distance > remaining) {
      phase += remaining;
      return wrapped;
    }
    time += distance / increment;
    const double since = time < 1.0 ? 1.0 - time : 0.0;
    switch (waveform_) {
    case Waveform::saw:
      apply_step(-2.0, since);
      break;
    case Waveform::pulse:
      // A wrap while already high (width swept below the phase) is no edge.
      if (event < 1.0 || !pulse_high_) {
        apply_step(event < 1.0 ? -2.0 : 2.0, since);
      }
      pulse_high_ = event >= 1.0;
      break;
    case Waveform::triangle:
      apply_ramp((event < 1.0 ? -8.0 : 8.0) * increment, since);
      break;
    case Waveform::sine:
      break;
    }
    remaining -= distance;
    phase = event < 1.0 ? event : 0.0;
    if (event >= 1.0) {
      wrapped = true;
      if (wrap != nullptr) {
        *wrap = {true, since};
      }
    }
  }
  return wrapped;
}

template <BlepKernel Kernel>
void BasicBlepOscillator<Kernel>::apply_step(double height, double since) noexcept {
  for (std::uint32_t tap = 0; tap < 2 * Kernel.half_width; ++tap) {
    const double offset = static_cast<double>(tap) - static_cast<double>(latency_frames);
    ring_[(position_ + tap - latency_frames) & ring_mask] +=
        static_cast<float>(height * tables_->step(offset + since));
  }
}

template <BlepKernel Kernel>
void BasicBlepOscillator<Kernel>::apply_ramp(double slope_change, double since) noexcept {
  if (slope_change == 0.0) {
    return;
  }
  for (std::uint32_t tap = 0; tap < 2 * Kernel.half_width; ++tap) {
    const double offset = static_cast<double>(tap) - static_cast<double>(latency_frames);
    ring_[(position_ + tap - latency_frames) & ring_mask] +=
        static_cast<float>(slope_change * tables_->ramp(offset + since));
  }
}

template class BasicBlepTables<standard_blep>;
template class BasicBlepTables<clean_blep>;
template class BasicBlepTables<clean_blep_2x>;
template class BasicBlepTables<clean_blep_4x>;
template class BasicBlepOscillator<standard_blep>;
template class BasicBlepOscillator<clean_blep>;
template class BasicBlepOscillator<clean_blep_2x>;
template class BasicBlepOscillator<clean_blep_4x>;

} // namespace oscillate::native_dsp::synth

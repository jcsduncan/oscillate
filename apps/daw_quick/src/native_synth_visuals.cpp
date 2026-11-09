#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/synth/bass_dsp.hpp"
#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/dx_envelope.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/fm.hpp"
#include "oscillate/native_dsp/synth/grain_dsp.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_filters.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::quick {
namespace {

using native_dsp::synth::FilterMode;

// One second-order analog section at normalised frequency w.
std::complex<double> section(FilterMode mode, double w, double q) {
  const std::complex<double> s(0.0, w);
  const std::complex<double> denominator = s * s + s / q + 1.0;
  switch (mode) {
  case FilterMode::highpass_12:
  case FilterMode::highpass_24:
    return s * s / denominator;
  case FilterMode::bandpass_12:
  case FilterMode::bandpass_24:
    return s / denominator;
  case FilterMode::notch_12:
  case FilterMode::notch_24:
    return (s * s + 1.0) / denominator;
  default:
    return 1.0 / denominator;
  }
}

std::optional<double> value_of(const native_dsp::EngineDescriptor& engine,
                               std::span<const double> values, std::string_view id) {
  const auto index = native_dsp::find_parameter(engine, id);
  if (!index || *index >= values.size()) {
    return std::nullopt;
  }
  return values[*index];
}

// The shared multimode SVF pairs and ladder at w = hz / cutoff, as the DSP
// derives them.
double multimode_db(FilterMode mode, double w, double resonance) {
  std::complex<double> h;
  if (mode == FilterMode::ladder_24) {
    const double k = resonance * native_dsp::synth::max_ladder_feedback;
    const std::complex<double> g = 1.0 / std::complex<double>(1.0, w);
    const std::complex<double> g4 = g * g * g * g;
    h = (1.0 + 0.5 * k) * g4 / (1.0 + k * g4);
  } else if (mode == FilterMode::lowpass_12 || mode == FilterMode::bandpass_12 ||
             mode == FilterMode::highpass_12 || mode == FilterMode::notch_12) {
    h = section(mode, w, native_dsp::synth::resonance_q(resonance, std::numbers::sqrt2 / 2.0));
  } else {
    h = section(mode, w, 0.5411961) *
        section(mode, w, native_dsp::synth::resonance_q(resonance, 1.3065630));
  }
  return 20.0 * std::log10(std::max(std::abs(h), 1e-6));
}

// Prism: its multimode SVF pairs and the ladder, as the DSP derives them.
class PrismVisuals final : public NativeSynthVisuals {
public:
  [[nodiscard]] double filter_response_db(const native_dsp::EngineDescriptor& engine,
                                          std::span<const double> values,
                                          double hz) const override {
    const auto mode_value = value_of(engine, values, "prism.filter.mode");
    const auto cutoff = value_of(engine, values, "prism.filter.cutoff");
    const auto resonance = value_of(engine, values, "prism.filter.resonance");
    if (!mode_value || !cutoff || !resonance || *cutoff <= 0.0) {
      return 0.0;
    }
    return multimode_db(static_cast<FilterMode>(static_cast<int>(*mode_value)), hz / *cutoff,
                        *resonance);
  }
};

const PrismVisuals prism_visuals;

// Ember: the OTA 4-pole with its uncompensated resonance (input gain
// 1 + 0.25k, so resonance costs bass) or the 12 dB SEM morph, as the DSP
// derives them from the same parameters.
class EmberVisuals final : public NativeSynthVisuals {
public:
  [[nodiscard]] double filter_response_db(const native_dsp::EngineDescriptor& engine,
                                          std::span<const double> values,
                                          double hz) const override {
    const auto type = value_of(engine, values, "ember.filter.type");
    const auto cutoff = value_of(engine, values, "ember.filter.cutoff");
    const auto resonance = value_of(engine, values, "ember.filter.resonance");
    const auto morph = value_of(engine, values, "ember.filter.morph");
    if (!type || !cutoff || !resonance || !morph || *cutoff <= 0.0) {
      return 0.0;
    }
    const double w = hz / *cutoff;
    const double r = std::clamp(*resonance, 0.0, 1.0);
    std::complex<double> h;
    if (*type < 0.5) {
      const double k = r * native_dsp::synth::max_ota_feedback;
      const std::complex<double> g = 1.0 / std::complex<double>(1.0, w);
      const std::complex<double> g4 = g * g * g * g;
      h = (1.0 + 0.25 * k) * g4 / (1.0 + k * g4);
    } else {
      const double q = native_dsp::synth::resonance_q(r, std::numbers::sqrt2 / 2.0);
      const auto low = section(FilterMode::lowpass_12, w, q);
      const auto high = section(FilterMode::highpass_12, w, q);
      const auto notch = low + high;
      const double m = std::clamp(*morph, 0.0, 1.0);
      h = m <= 0.5 ? low * (1.0 - 2.0 * m) + notch * (2.0 * m)
                   : notch * (2.0 - 2.0 * m) + high * (2.0 * m - 1.0);
    }
    return 20.0 * std::log10(std::max(std::abs(h), 1e-6));
  }
};

const EmberVisuals ember_visuals;

// Pulse pads in key order, as the kernel numbers them.
enum PulsePad : std::size_t {
  kick,
  kick2,
  snare,
  rim,
  clap,
  tom_low,
  chh,
  tom_mid,
  cowbell,
  tom_high,
  ohh,
  cymbal,
  pulse_pads
};
constexpr std::array<std::string_view, pulse_pads> pulse_pad_ids{
    "kick", "kick2",   "snare",   "rim",      "clap", "tom_low",
    "chh",  "tom_mid", "cowbell", "tom_high", "ohh",  "cymbal"};

struct PulsePadValues {
  double tune_scale;
  double decay;
  double tone;
  double character;
};

std::optional<PulsePadValues> pulse_pad(const native_dsp::EngineDescriptor& engine,
                                        std::span<const double> values, std::size_t pad) {
  if (engine.id != "pulse" || pad >= pulse_pads) {
    return std::nullopt;
  }
  const auto read = [&](std::string_view control) {
    return value_of(engine, values,
                    "pulse." + std::string(pulse_pad_ids[pad]) + "." + std::string(control));
  };
  const auto tune = read("tune");
  const auto decay = read("decay");
  const auto tone = read("tone");
  const auto character = read("character");
  if (!tune || !decay || !tone || !character) {
    return std::nullopt;
  }
  return PulsePadValues{std::exp2(*tune / 12.0), std::max(*decay, 1e-3), *tone, *character};
}

// decay_coefficient's law: -60 dB after `seconds`.
double fall(double t, double seconds) { return std::pow(1e-3, std::max(t, 0.0) / seconds); }

double pulse_amplitude(std::size_t pad, const PulsePadValues& v, double t) {
  constexpr double click = 0.003;
  switch (pad) {
  case kick:
  case kick2:
  case rim:
    return fall(t, v.decay) + v.character * fall(t, click);
  case snare:
    return (1.0 - 0.5 * v.character) * fall(t, 0.5 * v.decay) +
           (0.3 + v.character) * fall(t, v.decay);
  case clap: {
    // Four bursts 0.008 + 0.008 * character apart, the last with the tail.
    constexpr double bursts = 4.0;
    const double spacing = 0.008 + 0.008 * v.character;
    const double started = std::min(std::floor(t / spacing), bursts - 1.0);
    const double burst = fall(t - started * spacing, 0.006);
    const double tail_start = (bursts - 1.0) * spacing;
    const double tail = t >= tail_start ? 0.6 * fall(t - tail_start, v.decay) : 0.0;
    return std::max(burst, tail);
  }
  case cowbell:
    return 0.6 * fall(t, 0.04) + 0.4 * fall(t, v.decay);
  default:
    return fall(t, v.decay);
  }
}

} // namespace

std::vector<double> wavetable_frame_points(std::size_t table, std::size_t frame,
                                           std::size_t points) {
  const auto& tables = native_dsp::synth::wavetables();
  if (table >= tables.size() || frame >= native_dsp::synth::wavetable_frames || points == 0) {
    return {};
  }
  const auto& band = tables[table].levels[0];
  const float* samples = band.frame(frame);
  std::vector<double> result(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double x = static_cast<double>(i) * band.length / static_cast<double>(points);
    const auto index = std::min(static_cast<std::size_t>(x), std::size_t{band.length} - 1);
    const double t = x - static_cast<double>(index);
    result[i] = samples[index] + t * (samples[index + 1] - samples[index]);
  }
  return result;
}

std::vector<double> pulse_pad_envelope(const native_dsp::EngineDescriptor& engine,
                                       std::span<const double> values, std::size_t pad,
                                       double seconds, std::size_t points) {
  const auto v = pulse_pad(engine, values, pad);
  if (!v || points < 2 || seconds <= 0.0) {
    return {};
  }
  std::vector<double> result(points);
  double peak = 0.0;
  for (std::size_t i = 0; i < points; ++i) {
    result[i] = pulse_amplitude(pad, *v,
                                seconds * static_cast<double>(i) / static_cast<double>(points - 1));
    peak = std::max(peak, result[i]);
  }
  for (auto& point : result) {
    point /= peak;
  }
  return result;
}

std::vector<double> pulse_pad_pitch(const native_dsp::EngineDescriptor& engine,
                                    std::span<const double> values, std::size_t pad, double seconds,
                                    std::size_t points) {
  const auto v = pulse_pad(engine, values, pad);
  if (!v || points < 2 || seconds <= 0.0) {
    return {};
  }
  // ResonantBody: end + (start - end) * exp(-t / sweep).
  double end = 0.0;
  double excess = 0.0;
  double sweep = 0.0;
  switch (pad) {
  case kick:
    end = 50.0 * v->tune_scale;
    excess = end * 6.0 * v->tone;
    sweep = 0.03;
    break;
  case kick2:
    end = 55.0 * v->tune_scale;
    excess = end * 10.0 * v->tone;
    sweep = 0.012;
    break;
  case tom_low:
  case tom_mid:
  case tom_high:
    end = (pad == tom_low ? 90.0 : (pad == tom_mid ? 135.0 : 200.0)) * v->tune_scale;
    excess = end * 0.8 * v->tone;
    sweep = 0.06;
    break;
  default:
    return {};
  }
  std::vector<double> result(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double t = seconds * static_cast<double>(i) / static_cast<double>(points - 1);
    result[i] = end + excess * std::exp(-t / sweep);
  }
  return result;
}

double pulse_pad_band_db(const native_dsp::EngineDescriptor& engine, std::span<const double> values,
                         std::size_t pad, double hz) {
  const auto v = pulse_pad(engine, values, pad);
  if (!v || hz <= 0.0) {
    return 0.0;
  }
  const auto at = [hz](FilterMode mode, double cutoff, double q) {
    return section(mode, hz / cutoff, q);
  };
  const double scale = v->tune_scale;
  const double tone = v->tone;
  std::complex<double> h = 1.0;
  switch (pad) {
  case snare:
    h = at(FilterMode::highpass_12, 1'200.0, 0.7) *
        at(FilterMode::lowpass_12, 4'000.0 + 8'000.0 * tone, 0.7);
    break;
  case rim:
    h = at(FilterMode::highpass_12, 600.0, 0.7);
    break;
  case clap:
    h = at(FilterMode::bandpass_12, 1'100.0 * scale * (0.7 + 0.6 * tone), 1.5);
    break;
  case chh:
  case ohh:
    h = at(FilterMode::bandpass_12, 7'000.0 * scale * (0.6 + 0.8 * tone), 1.0) *
        at(FilterMode::highpass_12, 6'500.0 * scale, 0.7);
    break;
  case cymbal:
    h = at(FilterMode::bandpass_12, 4'500.0 * scale * (0.6 + 0.8 * tone), 0.8) *
        at(FilterMode::highpass_12, 3'000.0 * scale, 0.7);
    break;
  case cowbell:
    h = at(FilterMode::bandpass_12, 2'640.0 * scale * (0.8 + 0.4 * tone), 2.0);
    break;
  default:
    break;
  }
  return 20.0 * std::log10(std::max(std::abs(h), 1e-6));
}

namespace {

// Halo: the optional filter after the FM section (Off is flat), with the
// engine's type order Off, LP 12, LP 24, BP 12, HP 12, Ladder.
class HaloVisuals final : public NativeSynthVisuals {
public:
  [[nodiscard]] double filter_response_db(const native_dsp::EngineDescriptor& engine,
                                          std::span<const double> values,
                                          double hz) const override {
    const auto type = value_of(engine, values, "halo.filter.type");
    const auto cutoff = value_of(engine, values, "halo.filter.cutoff");
    const auto resonance = value_of(engine, values, "halo.filter.resonance");
    if (!type || !cutoff || !resonance || *cutoff <= 0.0) {
      return 0.0;
    }
    constexpr std::array<FilterMode, 6> modes{FilterMode::lowpass_12,  FilterMode::lowpass_12,
                                              FilterMode::lowpass_24,  FilterMode::bandpass_12,
                                              FilterMode::highpass_12, FilterMode::ladder_24};
    const int kind = static_cast<int>(*type);
    if (kind <= 0 || kind >= static_cast<int>(modes.size())) {
      return 0.0;
    }
    return multimode_db(modes[static_cast<std::size_t>(kind)], hz / *cutoff,
                        std::clamp(*resonance, 0.0, 1.0));
  }
};

const HaloVisuals halo_visuals;

// Torque: per model, the filter the kernel runs at the knob's cutoff (the
// envelope, keytracking and LFO move it from there).
//  Acid  - the diode ladder's linear model (ADR-0032), whose knob cutoff is
//          the resonant peak (wc = cutoff * sqrt 2), with the kernel's own
//          feedback and makeup from DiodeLadderCoefficients.
//  Moog  - the transistor ladder.
//  Reese - LP 24 or BP 12, then the notch comb mixed in, sketched at C2.
//  Sub   - LP 12 at half the resonance.
std::complex<double> diode_ladder_open_loop(std::complex<double> s) {
  // Solve (sI - A) y = e1 for y4 by elimination on the tridiagonal system.
  using C = std::complex<double>;
  const std::array<C, 4> diagonal{s + 1.0, s + 1.0, s + 1.0, s + 1.0};
  const std::array<C, 3> upper{-1.0, -0.5, -0.5};
  const std::array<C, 3> lower{-0.5, -0.5, -0.5};
  std::array<C, 4> pivot{};
  std::array<C, 4> rhs{1.0, 0.0, 0.0, 0.0};
  pivot[0] = diagonal[0];
  for (std::size_t i = 1; i < 4; ++i) {
    const C factor = lower[i - 1] / pivot[i - 1];
    pivot[i] = diagonal[i] - factor * upper[i - 1];
    rhs[i] -= factor * rhs[i - 1];
  }
  return rhs[3] / pivot[3];
}

class TorqueVisuals final : public NativeSynthVisuals {
public:
  [[nodiscard]] double filter_response_db(const native_dsp::EngineDescriptor& engine,
                                          std::span<const double> values,
                                          double hz) const override {
    const auto model = value_of(engine, values, "torque.model");
    const auto cutoff = value_of(engine, values, "torque.cutoff");
    const auto resonance = value_of(engine, values, "torque.resonance");
    if (!model || !cutoff || !resonance || *cutoff <= 0.0) {
      return 0.0;
    }
    const double r = std::clamp(*resonance, 0.0, 1.0);
    std::complex<double> h;
    switch (static_cast<int>(*model)) {
    case 0: {
      const auto k = native_dsp::synth::DiodeLadderCoefficients::make(*cutoff, 96'000.0, r);
      const double wc = *cutoff / native_dsp::synth::diode_ladder_resonant_ratio;
      const auto open = diode_ladder_open_loop({0.0, hz / wc});
      h = k.makeup * open / (1.0 + k.feedback * open);
      break;
    }
    case 1:
      return multimode_db(FilterMode::ladder_24, hz / *cutoff, r);
    case 2: {
      const auto filter = value_of(engine, values, "torque.reese.filter");
      const auto comb = value_of(engine, values, "torque.reese.comb");
      const auto mode = filter && *filter > 0.5 ? FilterMode::bandpass_12 : FilterMode::lowpass_24;
      const double w = hz / *cutoff;
      h = mode == FilterMode::bandpass_12
              ? section(mode, w, native_dsp::synth::resonance_q(r, std::numbers::sqrt2 / 2.0))
              : section(mode, w, 0.5411961) *
                    section(mode, w, native_dsp::synth::resonance_q(r, 1.3065630));
      // The kernel's comb: y = x + g y[n - T], output (1 - |g|) y, mixed in.
      const double mix = comb ? std::clamp(*comb, 0.0, 1.0) : 0.0;
      const double g = -0.75 * mix;
      const auto delay = std::polar(1.0, -2.0 * std::numbers::pi * hz / torque_comb_reference_hz);
      const auto combed = (1.0 - std::fabs(g)) / (1.0 - g * delay);
      h *= (1.0 - mix) + mix * combed;
      break;
    }
    default:
      return multimode_db(FilterMode::lowpass_12, hz / *cutoff, 0.5 * r);
    }
    return 20.0 * std::log10(std::max(std::abs(h), 1e-6));
  }
};

const TorqueVisuals torque_visuals;

// Drift: the post-grain multimode filter (LP 12, LP 24, BP 12 or HP 12), as
// the DSP derives it from the shared coefficients.
class DriftVisuals final : public NativeSynthVisuals {
public:
  [[nodiscard]] double filter_response_db(const native_dsp::EngineDescriptor& engine,
                                          std::span<const double> values,
                                          double hz) const override {
    const auto type = value_of(engine, values, "drift.filter.type");
    const auto cutoff = value_of(engine, values, "drift.filter.cutoff");
    const auto resonance = value_of(engine, values, "drift.filter.resonance");
    if (!type || !cutoff || !resonance || *cutoff <= 0.0) {
      return 0.0;
    }
    constexpr std::array<FilterMode, 4> modes{FilterMode::lowpass_12, FilterMode::lowpass_24,
                                              FilterMode::bandpass_12, FilterMode::highpass_12};
    const auto choice = static_cast<std::size_t>(std::clamp(*type, 0.0, 3.0));
    return multimode_db(modes[choice], hz / *cutoff, *resonance);
  }
};

const DriftVisuals drift_visuals;

// Naive oscillator shapes as BlepOscillator::naive draws them.
double torque_wave(int shape, double phase) {
  phase -= std::floor(phase);
  switch (shape) {
  case 1:
    return phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
  case 2:
    return phase < 0.5 ? 1.0 : -1.0;
  default:
    return 2.0 * phase - 1.0;
  }
}

} // namespace

FmAlgorithmLayout fm_algorithm_layout(std::size_t index) {
  FmAlgorithmLayout layout;
  const auto& table = native_dsp::synth::fm_algorithm_table();
  if (index >= table.size()) {
    return layout;
  }
  const auto& algorithm = table[index];
  constexpr std::size_t ops = native_dsp::synth::fm_operators;
  layout.valid = true;
  layout.modulates = algorithm.modulates;
  layout.carriers = algorithm.carriers;
  layout.feedback = algorithm.feedback;
  layout.feedback_from = algorithm.feedback_from;
  const auto heard = [&](std::size_t op) { return ((algorithm.carriers >> op) & 1U) != 0U; };
  const auto targets = [&](std::size_t from, std::size_t to) {
    return ((algorithm.modulates[from] >> to) & 1U) != 0U;
  };
  std::array<bool, ops> placed{};
  const auto free_at = [&](std::size_t self, int row, double column) {
    for (std::size_t other = 0; other < ops; ++other) {
      if (other != self && placed[other] && layout.row[other] == row &&
          std::fabs(layout.column[other] - column) < 0.75) {
        return false;
      }
    }
    return true;
  };
  // Targets always have lower numbers, so one ascending pass sees them first.
  double next_carrier = 0.0;
  for (std::size_t op = 0; op < ops; ++op) {
    if (heard(op)) {
      layout.row[op] = 0;
      layout.column[op] = next_carrier;
      next_carrier += 1.0;
    } else {
      int row = 0;
      double sum = 0.0;
      int count = 0;
      for (std::size_t to = 0; to < op; ++to) {
        if (targets(op, to)) {
          row = std::max(row, layout.row[to] + 1);
          sum += layout.column[to];
          ++count;
        }
      }
      double column = count > 0 ? sum / count : 0.0;
      while (!free_at(op, row, column)) {
        column += 1.0;
      }
      layout.row[op] = row;
      layout.column[op] = column;
    }
    placed[op] = true;
  }
  // Centre each carrier under the modulators feeding it, when that is free.
  for (std::size_t op = 0; op < ops; ++op) {
    if (!heard(op)) {
      continue;
    }
    double sum = 0.0;
    int count = 0;
    for (std::size_t from = op + 1; from < ops; ++from) {
      if (targets(from, op)) {
        sum += layout.column[from];
        ++count;
      }
    }
    if (count > 1 && free_at(op, 0, sum / count)) {
      layout.column[op] = sum / count;
    }
  }
  for (std::size_t op = 0; op < ops; ++op) {
    layout.columns = std::max(layout.columns, layout.column[op] + 1.0);
    layout.rows = std::max(layout.rows, layout.row[op] + 1);
  }
  return layout;
}

namespace {

// An ADSR run with the kernel's own envelope and curve through attack and
// decay, a short hold at sustain, then release.
EnvelopeSketch adsr_sketch(double attack, double decay, double sustain, double release,
                           double curve, std::size_t points) {
  EnvelopeSketch sketch;
  const double a = native_dsp::synth::Adsr::clamp_seconds(attack);
  const double d = native_dsp::synth::Adsr::clamp_seconds(decay);
  const double r = native_dsp::synth::Adsr::clamp_seconds(release);
  const double hold = std::max(0.15 * (a + d + r), 0.02);
  sketch.release_at = a + d + hold;
  sketch.seconds = sketch.release_at + r;
  // Run the real envelope at a rate that gives `points` samples.
  const double rate = static_cast<double>(points - 1) / sketch.seconds;
  native_dsp::synth::Adsr envelope;
  envelope.prepare(rate);
  envelope.set({a, d, std::clamp(sustain, 0.0, 1.0), r, curve});
  envelope.gate_on();
  sketch.points.reserve(points);
  sketch.points.push_back(0.0);
  bool released = false;
  for (std::size_t i = 1; i < points; ++i) {
    if (!released && static_cast<double>(i) / rate >= sketch.release_at) {
      envelope.gate_off();
      released = true;
    }
    sketch.points.push_back(envelope.next());
  }
  return sketch;
}

} // namespace

EnvelopeSketch halo_operator_envelope(const native_dsp::EngineDescriptor& engine,
                                      std::span<const double> values, std::size_t op,
                                      std::size_t points) {
  using native_dsp::synth::DxEnvelope;
  if (engine.id != "halo" || op >= native_dsp::synth::fm_operators || points < 2) {
    return {};
  }
  const std::string prefix = "halo.op" + std::to_string(op + 1) + ".";
  native_dsp::synth::DxEnvelopeSettings settings;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto rate = value_of(engine, values, prefix + "r" + std::to_string(i + 1));
    const auto level = value_of(engine, values, prefix + "l" + std::to_string(i + 1));
    if (!rate || !level) {
      return {};
    }
    settings.rates[i] = *rate;
    settings.levels[i] = *level;
  }
  settings.rate_scaling =
      static_cast<int>(value_of(engine, values, prefix + "rate_scale").value_or(0));
  constexpr int key = 60;
  std::array<double, 4> units{};
  for (std::size_t i = 0; i < 4; ++i) {
    units[i] = DxEnvelope::level_units(settings.levels[i]);
  }
  const auto time = [&](double from, double to, std::size_t rate) {
    return DxEnvelope::segment_seconds(from, to, settings.rates[rate], settings.rate_scaling, key);
  };
  const double rise =
      time(units[3], units[0], 0) + time(units[0], units[1], 1) + time(units[1], units[2], 2);
  const double release = time(units[2], units[3], 3);
  const double hold = std::max(0.15 * (rise + release), 0.02);

  EnvelopeSketch sketch;
  sketch.release_at = rise + hold;
  sketch.seconds = sketch.release_at + release;
  // Run the real envelope at a rate that gives `points` samples.
  const double rate = static_cast<double>(points - 1) / sketch.seconds;
  const auto scale = [](double u) {
    return std::clamp((u - DxEnvelope::floor_units) /
                          (DxEnvelope::full_units - DxEnvelope::floor_units),
                      0.0, 1.0);
  };
  DxEnvelope envelope;
  envelope.prepare(rate);
  envelope.reset();
  envelope.set(settings, key);
  envelope.gate_on();
  sketch.points.reserve(points);
  sketch.points.push_back(scale(envelope.units()));
  bool released = false;
  for (std::size_t i = 1; i < points; ++i) {
    if (!released && static_cast<double>(i) / rate >= sketch.release_at) {
      envelope.gate_off();
      released = true;
    }
    (void)envelope.next();
    sketch.points.push_back(scale(envelope.units()));
  }
  return sketch;
}

std::vector<double> torque_oscillator_cycle(const native_dsp::EngineDescriptor& engine,
                                            std::span<const double> values, std::size_t points) {
  std::vector<double> cycle;
  if (engine.id != "torque" || points < 2) {
    return cycle;
  }
  const auto get = [&](std::string_view id) { return value_of(engine, values, id).value_or(0.0); };
  const int model = static_cast<int>(get("torque.model"));
  cycle.assign(points, 0.0);
  const double n = static_cast<double>(points);
  constexpr double two_pi = 2.0 * std::numbers::pi;
  switch (model) {
  case 0:
    if (get("torque.acid.wave") > 0.5) {
      // The kernel's tilt: a one-pole high-pass whose corner tracks the note.
      const double ratio = 0.02 + 1.2 * get("torque.acid.tilt");
      const double coefficient = 1.0 - std::exp(-two_pi * ratio / n);
      native_dsp::synth::SquareTilt tilt;
      for (int settle = 0; settle < 8; ++settle) {
        for (std::size_t i = 0; i < points; ++i) {
          cycle[i] = tilt.process(torque_wave(2, static_cast<double>(i) / n), coefficient);
        }
      }
    } else {
      for (std::size_t i = 0; i < points; ++i) {
        cycle[i] = torque_wave(0, static_cast<double>(i) / n);
      }
    }
    break;
  case 1: {
    const double detune = get("torque.moog.detune") / 100.0;
    const std::array<double, 3> semis{0.0, get("torque.moog.interval2") + detune,
                                      get("torque.moog.interval3") - detune};
    const std::array<double, 3> levels{1.0, get("torque.moog.level2"), get("torque.moog.level3")};
    const std::array<int, 3> shapes{static_cast<int>(get("torque.moog.wave1")),
                                    static_cast<int>(get("torque.moog.wave2")),
                                    static_cast<int>(get("torque.moog.wave3"))};
    for (std::size_t i = 0; i < points; ++i) {
      const double p = static_cast<double>(i) / n;
      for (std::size_t o = 0; o < 3; ++o) {
        // The kernel starts oscillator o at phase 0.137 o.
        cycle[i] += levels[o] * torque_wave(shapes[o], 0.137 * static_cast<double>(o) +
                                                           p * std::exp2(semis[o] / 12.0));
      }
    }
    break;
  }
  case 2: {
    const auto count = static_cast<std::size_t>(std::clamp(get("torque.reese.voices"), 2.0, 7.0));
    const double detune = get("torque.reese.detune");
    const double sub = 0.8 * get("torque.reese.sub");
    constexpr double drift_seconds = 0.2;
    for (std::size_t v = 0; v < count; ++v) {
      const double position = -1.0 + 2.0 * static_cast<double>(v) / static_cast<double>(count - 1);
      const double ratio = std::exp2(position * detune / 2.0 / 1'200.0);
      const double start =
          0.137 * static_cast<double>(v) + torque_comb_reference_hz * (ratio - 1.0) * drift_seconds;
      for (std::size_t i = 0; i < points; ++i) {
        cycle[i] += torque_wave(0, start + static_cast<double>(i) / n * ratio);
      }
    }
    for (std::size_t i = 0; i < points; ++i) {
      cycle[i] = cycle[i] / std::sqrt(static_cast<double>(count)) +
                 sub * std::sin(std::numbers::pi * static_cast<double>(i) / n);
    }
    break;
  }
  default: {
    const int layer = static_cast<int>(get("torque.sub.layer"));
    const double level = layer == 0 ? 0.0 : get("torque.sub.layer_level");
    const double ratio = layer == 2 ? 2.0 : 1.5;
    for (std::size_t i = 0; i < points; ++i) {
      const double p = static_cast<double>(i) / n;
      cycle[i] = std::sin(two_pi * p) + level * std::sin(two_pi * ratio * p);
    }
    break;
  }
  }
  double peak = 0.0;
  for (const double v : cycle) {
    peak = std::max(peak, std::fabs(v));
  }
  if (peak > 0.0) {
    for (double& v : cycle) {
      v /= peak;
    }
  }
  return cycle;
}

std::vector<double> torque_pitch_drop(const native_dsp::EngineDescriptor& engine,
                                      std::span<const double> values, double seconds,
                                      std::size_t points) {
  std::vector<double> drop;
  const auto semitones = value_of(engine, values, "torque.sub.drop");
  const auto time = value_of(engine, values, "torque.sub.drop_time");
  if (engine.id != "torque" || points < 2 || seconds <= 0.0 || !semitones || !time) {
    return drop;
  }
  const double excess = std::exp2(*semitones / 12.0) - 1.0; // Over f0, in Hz ratio.
  drop.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double t = seconds * static_cast<double>(i) / static_cast<double>(points - 1);
    drop.push_back(12.0 * std::log2(1.0 + excess * std::exp(-t / std::max(*time, 1e-4))));
  }
  return drop;
}

std::vector<GrainPreview> drift_grain_cloud(const native_dsp::EngineDescriptor& engine,
                                            std::span<const double> values, double seconds,
                                            std::size_t max_grains) {
  std::vector<GrainPreview> grains;
  if (engine.id != "drift" || seconds <= 0.0 || max_grains == 0) {
    return grains;
  }
  const auto get = [&](std::string_view id) { return value_of(engine, values, id).value_or(0.0); };
  // The settings the kernel derives at rest (LFOs centred, wheel down).
  const double blur = std::clamp(get("drift.blur") + get("drift.perf.wheel"), 0.0, 1.0);
  const double size = get("drift.grain.size");
  const double density = std::max(get("drift.grain.density"), 1e-3);
  const double chaos = std::clamp(get("drift.grain.chaos") + 0.5 * blur, 0.0, 1.0);
  const bool synced = get("drift.sync") > 0.5;
  // Synced divisions are drawn at 120 BPM in 4/4 (the engine's no-tempo
  // default), from the shared division table.
  const auto division_index = static_cast<std::size_t>(std::clamp(
      get("drift.sync.division"), 0.0, static_cast<double>(native_dsp::sync_division_count - 1)));
  const double division_seconds =
      native_dsp::sync_seconds(division_index, native_dsp::Transport{}, 48'000.0);
  const double lag = std::max(0.0, get("drift.grain.position"));
  const double scan = get("drift.grain.scan");
  const double spray = get("drift.grain.spray") + 0.5 * blur;
  const double pitch = get("drift.grain.pitch");
  const auto intervals = static_cast<int>(get("drift.grain.intervals"));
  const double harmony = get("drift.grain.interval_amount");
  const double jitter = get("drift.grain.jitter") + 50.0 * blur;
  const double reverse = std::clamp(get("drift.grain.reverse") + 0.4 * blur, 0.0, 1.0);
  const double spread = get("drift.grain.spread");
  const bool frozen = get("drift.freeze") > 0.5;
  constexpr std::array<double, 3> octaves{-12.0, 12.0, 24.0};
  constexpr std::array<double, 3> fifths{7.0, 12.0, -5.0};
  constexpr std::array<double, 5> pentatonic{2.0, 4.0, 7.0, 9.0, 12.0};

  // The first note's seed: slot 0, age 1.
  native_dsp::synth::NoiseSource random;
  random.reset(104'729U + 1U);
  double t = 0.0;
  for (std::size_t k = 0; grains.size() < max_grains && t < seconds; ++k) {
    // The kernel's draw order: six per grain, then the free spacing.
    const double u_interval = random.white();
    const double u_choice = random.white();
    const double u_jitter = random.white();
    const double u_reverse = random.white();
    const double u_spray = random.white();
    const double u_pan = random.white();
    double semitones = 0.0;
    if (intervals > 0 && 0.5 * (u_interval + 1.0) < harmony) {
      const double pick = 0.5 * (u_choice + 1.0);
      if (intervals == 1) {
        semitones = octaves[std::min<std::size_t>(2, static_cast<std::size_t>(pick * 3.0))];
      } else if (intervals == 2) {
        semitones = fifths[std::min<std::size_t>(2, static_cast<std::size_t>(pick * 3.0))];
      } else {
        semitones = pentatonic[std::min<std::size_t>(4, static_cast<std::size_t>(pick * 5.0))];
      }
    }
    // Lag drifts with the writer (1) less the scan; frozen, the writer stops.
    const double drift = ((frozen ? 0.0 : 1.0) - scan) * t;
    double behind = lag + drift;
    behind -= native_dsp::synth::drift::buffer_seconds *
              std::floor(behind / native_dsp::synth::drift::buffer_seconds);
    GrainPreview grain;
    grain.time = t;
    // Frozen, grains wrap round the held moment, as the scheduler does;
    // live, they clamp to the newest audio.
    const double span = native_dsp::synth::drift::buffer_seconds;
    const double placed = behind - spray * u_spray;
    grain.position =
        frozen ? placed - span * std::floor(placed / span) : std::clamp(placed, 0.0, span);
    grain.semitones = pitch + semitones + jitter * u_jitter / 100.0;
    grain.length = size;
    grain.reverse = 0.5 * (u_reverse + 1.0) < reverse;
    grain.pan = std::clamp(spread * u_pan, -1.0, 1.0);
    grains.push_back(grain);
    if (synced) {
      t = static_cast<double>(k + 1) * division_seconds;
    } else {
      const double spacing = (1.0 / density) * (1.0 + chaos * random.white());
      t += std::max(spacing, 1.0 / 48'000.0);
    }
  }
  return grains;
}

std::vector<PartialLine> drift_partial_spectrum(const native_dsp::EngineDescriptor& engine,
                                                std::span<const double> values) {
  std::vector<PartialLine> lines;
  if (engine.id != "drift") {
    return lines;
  }
  const double tilt = value_of(engine, values, "drift.partials.tilt").value_or(0.0);
  const double odd = value_of(engine, values, "drift.partials.odd").value_or(0.0);
  const double stretch = value_of(engine, values, "drift.partials.stretch").value_or(0.0);
  double peak = 0.0;
  for (std::size_t n = 1; n <= native_dsp::synth::PartialBank::count; ++n) {
    // The bank's law: n^-tilt, odd partials gain and even ones lose with odd.
    const double weight = n == 1 ? 1.0 : (n % 2 == 1 ? 1.0 + odd : 1.0 - odd);
    const double level = std::pow(static_cast<double>(n), -tilt) * std::max(0.0, weight);
    lines.push_back({native_dsp::synth::PartialBank::ratio(n, stretch), level});
    peak = std::max(peak, level);
  }
  for (auto& line : lines) {
    line.level = peak > 0.0 ? line.level / peak : 0.0;
  }
  return lines;
}

std::vector<double> drift_grain_window(int shape, std::size_t points) {
  std::vector<double> window;
  if (shape < 0 || shape > 3 || points < 2) {
    return window;
  }
  window.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    window.push_back(native_dsp::synth::GrainCloud::window(
        static_cast<native_dsp::synth::GrainShape>(shape),
        static_cast<double>(i) / static_cast<double>(points - 1)));
  }
  return window;
}

EnvelopeSketch drift_amp_envelope(const native_dsp::EngineDescriptor& engine,
                                  std::span<const double> values, std::size_t points) {
  if (engine.id != "drift" || points < 2) {
    return {};
  }
  const auto attack = value_of(engine, values, "drift.amp.attack");
  const auto decay = value_of(engine, values, "drift.amp.decay");
  const auto sustain = value_of(engine, values, "drift.amp.sustain");
  const auto release = value_of(engine, values, "drift.amp.release");
  if (!attack || !decay || !sustain || !release) {
    return {};
  }
  return adsr_sketch(*attack, *decay, *sustain, *release, drift_envelope_curve, points);
}

const NativeSynthVisuals* visuals_for(std::string_view engine_id) noexcept {
  if (engine_id == "drift") {
    return &drift_visuals;
  }
  if (engine_id == "torque") {
    return &torque_visuals;
  }
  if (engine_id == "prism") {
    return &prism_visuals;
  }
  if (engine_id == "ember") {
    return &ember_visuals;
  }
  if (engine_id == "halo") {
    return &halo_visuals;
  }
  return nullptr;
}

} // namespace oscillate::quick

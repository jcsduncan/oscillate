#include "oscillate/native_dsp/effects/contour.hpp"

#include "contour_presets.hpp"
#include "contour_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/vintage_filters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::contour {
namespace {

using contour_table::index_of;
using contour_table::Model;
using synth::FilterMode;

namespace p {
constexpr std::size_t model = index_of("contour.model");
constexpr std::size_t cutoff = index_of("contour.cutoff");
constexpr std::size_t resonance = index_of("contour.resonance");
constexpr std::size_t drive = index_of("contour.drive");
constexpr std::size_t morph = index_of("contour.morph");
constexpr std::size_t lfo_shape = index_of("contour.lfo.shape");
constexpr std::size_t lfo_rate = index_of("contour.lfo.rate");
constexpr std::size_t lfo_sync = index_of("contour.lfo.sync");
constexpr std::size_t lfo_division = index_of("contour.lfo.division");
constexpr std::size_t lfo_depth = index_of("contour.lfo.depth");
constexpr std::size_t lfo_stereo = index_of("contour.lfo.stereo");
constexpr std::size_t env_amount = index_of("contour.env.amount");
constexpr std::size_t env_attack = index_of("contour.env.attack");
constexpr std::size_t env_release = index_of("contour.env.release");
constexpr std::size_t env_source = index_of("contour.env.source");
constexpr std::size_t mix = index_of("contour.mix");
constexpr std::size_t output = index_of("contour.output");
} // namespace p

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
using native_dsp::dsp::db_to_gain;

[[nodiscard]] bool is_comb(int model) noexcept {
  return model == Model::comb_plus || model == Model::comb_minus;
}
[[nodiscard]] FilterMode svf_mode(int model) noexcept {
  switch (model) {
  case Model::lp12:
    return FilterMode::lowpass_12;
  case Model::bp12:
    return FilterMode::bandpass_12;
  case Model::hp12:
    return FilterMode::highpass_12;
  case Model::hp24:
    return FilterMode::highpass_24;
  case Model::notch:
    return FilterMode::notch_12;
  case Model::ladder:
    return FilterMode::ladder_24;
  default:
    return FilterMode::lowpass_24;
  }
}

// One channel's filters for every model and its 2x resampling.
struct Channel {
  synth::BasicUpsampler2x<double> up;
  synth::BasicDownsampler2x<double> down;
  synth::MultimodeFilter svf;
  synth::BasicOtaLadder<double> ota;
  synth::BasicSemFilter<double> sem;
  synth::CombFilter comb;

  void prepare(double rate) { comb.prepare(rate, 20.0); }
  void reset() noexcept {
    up.reset();
    down.reset();
    svf.reset();
    ota.reset();
    sem.reset();
    comb.reset();
  }
  // One base-rate sample through `model` at cutoff `hz`. `aligned` is x
  // through the same resampling as the oversampled path.
  [[nodiscard]] double run(int model, double x, double aligned, double hz, double resonance,
                           double drive, double morph, double rate) noexcept {
    if (is_comb(model)) {
      // Combs run at the base rate on the aligned signal, so their output
      // lines up with the dry path; their tuning is set per control tick.
      return comb.process(aligned);
    }
    const double g = synth::prewarp(hz / (2.0 * rate));
    double a = 0.0;
    double b = 0.0;
    up.process(x, a, b);
    if (model == Model::ota) {
      const auto c = synth::OtaCoefficients::make(g, resonance, 0.6 + 3.4 * (drive - 1.0) / 15.0);
      a = ota.process(a, c);
      b = ota.process(b, c);
    } else if (model == Model::sem) {
      const auto c = synth::SemCoefficients::make(g, resonance, morph);
      const double makeup = 1.0 / std::sqrt(drive);
      a = sem.process(synth::soft_clip(a * drive) * makeup, c);
      b = sem.process(synth::soft_clip(b * drive) * makeup, c);
    } else {
      const auto c =
          synth::make_filter_coefficients(svf_mode(model), hz / (2.0 * rate), resonance, drive);
      a = svf.process(a, c);
      b = svf.process(b, c);
    }
    return down.process(a, b);
  }
};

class ContourKernel final : public EffectKernel {
public:
  explicit ContourKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames) {
    for (auto* set : {&channels_, &fading_}) {
      for (auto& channel : *set) {
        channel.prepare(sample_rate);
      }
    }
    for (auto& lfo : lfo_) {
      lfo.prepare(control_rate_);
    }
    fade_.prepare(sample_rate);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport& transport) noexcept override {
    const int model = as_int(v[p::model]);
    if (!primed_) {
      model_ = model;
    } else if (model != model_ && !fade_.active()) {
      // Live switch (ADR-0038): the old model runs on from a copy of its
      // state; the new model starts clean unless it shares the old's.
      fading_ = channels_;
      fading_model_ = model_;
      if (is_comb(model) != is_comb(model_) || family(model) != family(model_)) {
        for (auto& channel : channels_) {
          channel.svf.reset();
          channel.ota.reset();
          channel.sem.reset();
          channel.comb.reset();
        }
      }
      model_ = model;
      fade_.start();
    }
    resonance_ = v[p::resonance];
    drive_ = db_to_gain(v[p::drive]);
    morph_ = v[p::morph];
    cutoff_ = v[p::cutoff];

    // LFO at control rate; the right channel runs `stereo` degrees ahead.
    const auto shape = static_cast<synth::LfoShape>(as_int(v[p::lfo_shape]));
    for (auto& lfo : lfo_) {
      lfo.set_shape(shape);
    }
    const double offset = v[p::lfo_stereo] / 360.0;
    if (v[p::lfo_sync] != 0.0 && transport.beats_per_frame > 0.0) {
      cycles_ = sync_cycles(static_cast<std::size_t>(v[p::lfo_division]), transport);
    } else {
      cycles_ += v[p::lfo_rate] / control_rate_;
    }
    const std::array<double, 2> lfo{lfo_[0].next_at(cycles_), lfo_[1].next_at(cycles_ + offset)};
    for (std::size_t c = 0; c < 2; ++c) {
      lfo_from_[c] = primed_ ? lfo_to_[c] : lfo[c] * v[p::lfo_depth];
      lfo_to_[c] = lfo[c] * v[p::lfo_depth];
    }
    lfo_step_ = 1.0 / std::max<double>(1.0, frames);
    lfo_position_ = 0.0;

    env_amount_ = v[p::env_amount];
    env_attack_ = 1.0 - std::exp(-1.0 / (v[p::env_attack] * 1e-3 * rate_));
    env_release_ = 1.0 - std::exp(-1.0 / (v[p::env_release] * 1e-3 * rate_));
    env_sidechain_ = v[p::env_source] != 0.0;

    // The comb is tuned per tick from the cutoff with its LFO offset.
    if (is_comb(model_) || (fade_.active() && is_comb(fading_model_))) {
      for (std::size_t c = 0; c < 2; ++c) {
        const double hz =
            std::clamp(cutoff_ * std::exp2((lfo_to_[c] + env_semitones()) / 12.0), 20.0, 5'000.0);
        const double feedback = 0.97 * resonance_;
        channels_[c].comb.set(hz, model_ == Model::comb_minus ? -feedback : feedback,
                              primed_ ? frames : 0);
        fading_[c].comb.set(hz, fading_model_ == Model::comb_minus ? -feedback : feedback,
                            primed_ ? frames : 0);
      }
    }

    const double gain = db_to_gain(v[p::output]);
    mix_target_ = v[p::mix];
    gain_target_ = gain;
    if (!primed_) {
      mix_ = mix_target_;
      gain_ = gain_target_;
    }
    ramp_step_ = 1.0 / std::max<double>(1.0, frames);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    const double mix_step = (mix_target_ - mix_) * ramp_step_;
    const double gain_step = (gain_target_ - gain_) * ramp_step_;
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const std::array<double, 2> x{audio[i].left, audio[i].right};
      // Envelope follower on the input or the sidechain; a sidechain source
      // with nothing routed to it is silent.
      double level = 0.0;
      if (!env_sidechain_) {
        level = std::max(std::fabs(audio[i].left), std::fabs(audio[i].right));
      } else if (!sidechain.empty()) {
        level = std::max(std::fabs(sidechain[i].left), std::fabs(sidechain[i].right));
      }
      envelope_ += (level > envelope_ ? env_attack_ : env_release_) * (level - envelope_);
      lfo_position_ = std::min(1.0, lfo_position_ + lfo_step_);
      mix_ += mix_step;
      gain_ += gain_step;
      const double weight = fade_.active() ? fade_.next() : 0.0;
      std::array<double, 2> y{};
      for (std::size_t c = 0; c < 2; ++c) {
        const double lfo = lfo_from_[c] + (lfo_to_[c] - lfo_from_[c]) * lfo_position_;
        const double hz =
            std::clamp(cutoff_ * std::exp2((lfo + env_semitones()) / 12.0), 20.0, 20'000.0);
        // The dry signal through the same resampling as the wet path.
        const double aligned = aligned_dry(c, x[c]);
        double wet = channels_[c].run(model_, x[c], aligned, hz, resonance_, drive_, morph_, rate_);
        if (weight > 0.0) {
          const double old =
              fading_[c].run(fading_model_, x[c], aligned, hz, resonance_, drive_, morph_, rate_);
          wet += (old - wet) * weight;
        }
        y[c] = (aligned + (wet - aligned) * mix_) * gain_;
        if (c == 0) {
          last_cutoff_ = hz;
        }
      }
      audio[i].left = static_cast<float>(y[0]);
      audio[i].right = static_cast<float>(y[1]);
    }
  }

  void reset() noexcept override {
    for (auto* set : {&channels_, &fading_}) {
      for (auto& channel : *set) {
        channel.reset();
      }
    }
    for (auto& aligner : aligners_) {
      aligner.up.reset();
      aligner.down.reset();
    }
    fade_.stop();
    envelope_ = 0.0;
    cycles_ = 0.0;
    for (std::size_t c = 0; c < 2; ++c) {
      lfo_[c].reset(0.0, static_cast<std::uint32_t>(17 + c));
    }
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::cutoff_hz] = static_cast<float>(last_cutoff_);
    out[telemetry::envelope] = static_cast<float>(envelope_);
    out[telemetry::lfo] = static_cast<float>(lfo_to_[0]);
    return telemetry::size;
  }

private:
  struct Aligner {
    synth::BasicUpsampler2x<double> up;
    synth::BasicDownsampler2x<double> down;
  };

  // Filter families share state: switching inside one keeps it warm.
  [[nodiscard]] static int family(int model) noexcept {
    if (is_comb(model)) {
      return 3;
    }
    if (model == Model::ota) {
      return 1;
    }
    if (model == Model::sem) {
      return 2;
    }
    return 0;
  }

  // The envelope's cutoff offset: its level in dB over a 60 dB range.
  [[nodiscard]] double env_semitones() const noexcept {
    if (env_amount_ == 0.0) {
      return 0.0;
    }
    const double db = 20.0 * std::log10(envelope_ + 1e-9);
    return env_amount_ * std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
  }

  [[nodiscard]] double aligned_dry(std::size_t c, double x) noexcept {
    double a = 0.0;
    double b = 0.0;
    aligners_[c].up.process(x, a, b);
    return aligners_[c].down.process(a, b);
  }

  double rate_;
  double control_rate_;
  std::array<Channel, 2> channels_{};
  std::array<Channel, 2> fading_{};
  std::array<Aligner, 2> aligners_{};
  synth::SwitchFade fade_;
  int model_ = Model::lp24;
  int fading_model_ = Model::lp24;
  std::array<synth::Lfo, 2> lfo_{};
  double cycles_ = 0.0;
  std::array<double, 2> lfo_from_{};
  std::array<double, 2> lfo_to_{};
  double lfo_step_ = 0.0;
  double lfo_position_ = 0.0;
  double cutoff_ = 1'200.0;
  double resonance_ = 0.3;
  double drive_ = 1.0;
  double morph_ = 0.5;
  double envelope_ = 0.0;
  double env_amount_ = 0.0;
  double env_attack_ = 0.0;
  double env_release_ = 0.0;
  bool env_sidechain_ = false;
  double mix_ = 1.0;
  double mix_target_ = 1.0;
  double gain_ = 1.0;
  double gain_target_ = 1.0;
  double ramp_step_ = 0.0;
  double last_cutoff_ = 1'200.0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<ContourKernel>(sample_rate);
}

// A resonant filter rings for about Q / (pi f); a comb for its feedback's
// repeats at the lowest pitch it can reach.
std::uint64_t tail_frames(std::span<const double> v, double sample_rate) {
  const int model = as_int(v[p::model]);
  const double resonance = v[p::resonance];
  double seconds = 0.0;
  if (is_comb(model)) {
    const double feedback = std::max(1e-3, 0.97 * resonance);
    const double low = std::max(
        20.0, v[p::cutoff] * std::exp2(-(v[p::lfo_depth] + std::fabs(v[p::env_amount])) / 12.0));
    seconds = std::log(1e-3) / std::log(feedback) / low;
  } else {
    const double q = synth::resonance_q(resonance, 0.707);
    seconds = q / (std::numbers::pi * 20.0) * 6.9;
  }
  return static_cast<std::uint64_t>(std::ceil((seconds + 0.05) * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"contour", "Contour", 1, contour_table::parameters,
                                         contour_presets::presets};
const EffectEngine contour_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

const EffectEngine& engine() { return contour_engine; }

} // namespace oscillate::native_dsp::effects::contour

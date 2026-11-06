#include "oscillate/native_dsp/synth/nova/filter.hpp"

#include "synth/nova/filter_character.hpp"
#include "synth/nova/filter_comb.hpp"
#include "synth/nova/filter_impl.hpp"
#include "synth/nova/filter_ladder.hpp"
#include "synth/nova/filter_parts.hpp"
#include "synth/nova/filter_svf.hpp"

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Nova's voice filters (spec 08 §4, contract 08a §2.4).
//
// Every model is a zero-delay-feedback (trapezoidal) structure, so its small-
// signal response is the bilinear transform of an analog prototype and
// filter_response_db() is exact. Character comes from memoryless
// nonlinearities placed where they bound the feedback without moving the
// resonance: the ladders saturate their summing input and their stage
// inputs (never the integrator states), so self-oscillation stays on pitch
// at any level; the diode ladder adds signal-dependent branch conductances;
// the clean SVFs damp themselves harder as their band state grows.
//
// The models live by family: the ladders (filter_ladder), the clean SVFs
// (filter_svf), the character models (filter_character) and the delay
// lines (filter_comb), sharing filter_parts.hpp. This file dispatches to
// them and runs the shared drive, ramps and model crossfade.
namespace oscillate::native_dsp::synth::nova {
namespace {

using namespace filter_detail;

constexpr std::array<std::string_view, filter_model_count> model_names{
    "Ladder 24", "Ladder 12", "Diode",   "MG Low",   "OTA",       "LP 12",   "LP 24", "BP 12",
    "BP 24",     "HP 12",     "HP 24",   "Notch",    "Peak",      "Formant", "Vowel", "Comb +",
    "Comb −",    "Phaser",    "Flanger", "LP→BP→HP", "Dual Peak", "Ring"};

// soft_clip's rational form reaches its rail at this input.
constexpr double soft_clip_knee = 3.0;

// d/dx of soft_clip, for normalising a biased saturator's slope.
[[nodiscard]] double soft_clip_slope(double x) noexcept {
  if (std::fabs(x) >= soft_clip_knee) {
    return 0.0;
  }
  const double square = x * x;
  const double denominator = 27.0 + 9.0 * square;
  return ((27.0 + 3.0 * square) * denominator - 18.0 * square * (27.0 + square)) /
         (denominator * denominator);
}

// ---- Drive ----------------------------------------------------------------------------

// The pre-filter drive. 0 dB is exactly clean; the saturator fades in over
// the first 3 dB. Up to about 15 dB small signals keep unity gain and only
// peaks round off; beyond, quiet signals gain at most ~+4 dB while full-
// scale ones lose ~3 dB, so the knob adds density rather than level.
struct DriveCurve {
  double mix = 0.0;
  double gain = 1.0;
  double makeup = 1.0;
  double bias = 0.0; // Asymmetry for even harmonics; its DC is blocked.
};

// The drive's saturator fades in over drive_fade_db; its input is scaled by
// drive_input_scale of the drive gain; makeup follows drive_makeup_law of
// the log gain above unity; the bias reaches drive_max_bias at full drive.
constexpr double drive_fade_db = 3.0;
constexpr double drive_input_scale = 0.18;
constexpr double drive_makeup_law = 0.6;
constexpr double drive_max_bias = 0.15;

[[nodiscard]] DriveCurve drive_curve(double db) noexcept {
  DriveCurve d;
  d.mix = std::min(1.0, db / drive_fade_db);
  d.gain = drive_input_scale * db_to_gain(db);
  d.makeup = d.gain <= 1.0 ? 1.0 / d.gain : (1.0 + drive_makeup_law * std::log(d.gain)) / d.gain;
  d.bias = drive_max_bias * db / max_drive_db;
  return d;
}

[[nodiscard]] double drive_small_signal_gain(double db) noexcept {
  const DriveCurve d = drive_curve(db);
  return 1.0 - d.mix + d.mix * d.gain * d.makeup;
}

[[nodiscard]] bool uses_delay(FilterModel model) noexcept {
  return model == FilterModel::comb_plus || model == FilterModel::comb_minus ||
         model == FilterModel::flanger;
}

// The magnitude response never reads below this (-240 dB).
constexpr double magnitude_floor = 1e-12;
constexpr double db_per_decade = 20.0;
// Coefficients follow the ramps at no less than this rate, refreshing at
// most every max_refresh_interval frames.
constexpr double min_refresh_rate = 44'100.0;
constexpr double max_refresh_interval = 4.0;
// The comb lines: a power of two from min_delay_size, holding the longest
// period and delay_spare_frames for the interpolator; one per slot and
// channel.
constexpr std::size_t min_delay_size = 16;
constexpr std::size_t delay_spare_frames = 8;
constexpr std::size_t channels = 2;
constexpr std::size_t delay_lines = 2 * channels;
// The drive's even part loses its DC above this.
constexpr double dc_block_hz = 10.0;
// Each slot's noise generators start from these (offset by the slot).
constexpr std::uint32_t left_noise_seed = 0x2545F491U;
constexpr std::uint32_t right_noise_seed = 0x9E3779B9U;

} // namespace

std::string_view filter_model_name(FilterModel model) noexcept {
  const auto index = static_cast<std::size_t>(model);
  return index < model_names.size() ? model_names[index] : std::string_view{};
}

// ---- Models ---------------------------------------------------------------------------

void VoiceFilter::Impl::derive(FilterModel model, const Params& p, double rate,
                               Coefficients& c) noexcept {
  c = Coefficients{};
  switch (family_of(model)) {
  case Family::ladder:
    derive_ladder(model, p, rate, c);
    return;
  case Family::svf:
    derive_svf(model, p, rate, c);
    return;
  case Family::character:
    derive_character(model, p, rate, c);
    return;
  case Family::comb:
    derive_comb(model, p, rate, c);
    return;
  }
}

double VoiceFilter::Impl::run(Slot& slot, std::size_t channel, double x, float* line,
                              std::size_t mask) noexcept {
  Memory& m = slot.memory[channel];
  const Coefficients& c = slot.c;
  double y = x;
  switch (slot.model) {
  case FilterModel::ladder24:
  case FilterModel::ladder12:
  case FilterModel::mg_low:
  case FilterModel::ota:
    y = ladder(slot.model, c, m, slot.noise[channel], x);
    break;
  case FilterModel::diode:
    y = diode(c, m, slot.noise[channel], x);
    break;
  case FilterModel::lp12:
  case FilterModel::lp24:
  case FilterModel::bp12:
  case FilterModel::bp24:
  case FilterModel::hp12:
  case FilterModel::hp24:
  case FilterModel::notch:
  case FilterModel::peak:
    y = clean(slot.model, c, m, x);
    break;
  case FilterModel::comb_plus:
  case FilterModel::comb_minus:
  case FilterModel::flanger:
    y = line == nullptr ? x : comb(c, m, x, line, mask, slot.write);
    break;
  default:
    y = character(slot.model, c, m, x);
    break;
  }
  return y;
}

// The small-signal response: every model is trapezoidal, so its
// transfer function is its analog prototype at s = j tan(w / 2) / g.
VoiceFilter::Impl::Complex VoiceFilter::Impl::response(FilterModel model, const Coefficients& c,
                                                       double hz, double rate) noexcept {
  const double w = two_pi * std::clamp(hz, 0.0, response_top * rate) / rate;
  const ResponsePoint at{w, std::tan(0.5 * w)};
  switch (family_of(model)) {
  case Family::ladder:
    return response_ladder(model, c, at);
  case Family::svf:
    return response_svf(model, c, at);
  case Family::character:
    return response_character(model, c, at);
  case Family::comb:
    return response_comb(c, at);
  }
  return 1.0;
}

struct FilterResponseProbe {
  [[nodiscard]] static double db(const FilterSettings& settings, double frequency_hz,
                                 double sample_rate) {
    const double rate = supported_rate(sample_rate);
    const Params p{clamped(settings.cutoff_hz, min_cutoff_hz, max_cutoff_hz, Params{}.cutoff_hz),
                   clamped(settings.resonance, 0.0, 1.0, 0.0),
                   clamped(settings.drive_db, 0.0, max_drive_db, 0.0),
                   clamped(settings.fat, 0.0, 1.0, 0.0)};
    const auto index = static_cast<std::size_t>(settings.model);
    const FilterModel model = index < filter_model_count ? settings.model : FilterModel::ladder24;
    VoiceFilter::Coefficients c;
    VoiceFilter::Impl::derive(model, p, rate, c);
    const double magnitude = std::abs(VoiceFilter::Impl::response(model, c, frequency_hz, rate)) *
                             drive_small_signal_gain(p.drive_db);
    return db_per_decade * std::log10(std::max(magnitude, magnitude_floor));
  }
};

double filter_response_db(const FilterSettings& settings, double frequency_hz, double sample_rate) {
  return FilterResponseProbe::db(settings, frequency_hz, sample_rate);
}

// ---- VoiceFilter ----------------------------------------------------------------------

void VoiceFilter::prepare(double sample_rate) {
  sample_rate_ = supported_rate(sample_rate);
  // Coefficients follow the ramps at no less than ~44 kHz: every frame at
  // base rates, every second or fourth when the voice oversamples.
  interval_ = static_cast<std::uint32_t>(
      std::clamp(sample_rate_ / min_refresh_rate, 1.0, max_refresh_interval));
  fade_length_ = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(fade_seconds * sample_rate_)));
  std::size_t size = min_delay_size;
  const auto longest =
      static_cast<std::size_t>(std::ceil(sample_rate_ / lowest_delay_hz)) + delay_spare_frames;
  while (size < longest) {
    size <<= 1U;
  }
  delay_size_ = size;
  delay_.assign(delay_lines * size, 0.0F);
  dc_pole_ = std::exp(-two_pi * dc_block_hz / sample_rate_);
  reset();
}

void VoiceFilter::clear(std::size_t slot) noexcept {
  auto& s = slots_[slot];
  for (auto& memory : s.memory) {
    memory.fill(0.0);
  }
  s.noise = {left_noise_seed + static_cast<std::uint32_t>(slot),
             right_noise_seed + static_cast<std::uint32_t>(slot)};
  s.write = 0;
  if (delay_size_ > 0 && delay_.size() >= delay_lines * delay_size_) {
    std::fill_n(delay_.begin() + static_cast<std::ptrdiff_t>(channels * slot * delay_size_),
                channels * delay_size_, 0.0F);
  }
}

void VoiceFilter::reset() noexcept {
  clear(0);
  clear(1);
  active_ = 0;
  slots_[0].model = target_model_;
  fade_ = 0;
  ramp_ = 0;
  blend_ = false;
  value_ = goal_;
  step_ = {};
  primed_ = false;
  dirty_ = true;
  tick_ = 0;
  dc_in_ = {};
  dc_out_ = {};
}

void VoiceFilter::set(const FilterSettings& settings, std::uint32_t frames) noexcept {
  const double cutoff =
      clamped(settings.cutoff_hz, min_cutoff_hz, max_cutoff_hz, std::exp2(goal_[0]));
  goal_ = {std::log2(cutoff), clamped(settings.resonance, 0.0, 1.0, goal_[1]),
           clamped(settings.drive_db, 0.0, max_drive_db, goal_[2]),
           clamped(settings.fat, 0.0, 1.0, goal_[3])};
  if (!primed_ || frames == 0) {
    value_ = goal_;
    step_ = {};
    ramp_ = 0;
  } else {
    for (std::size_t i = 0; i < value_.size(); ++i) {
      step_[i] = (goal_[i] - value_[i]) / static_cast<double>(frames);
    }
    ramp_ = frames;
  }
  // Settings that hold still ramp by zero steps: process() keeps the ramp's
  // refresh timing but derives the (unchanged) coefficients only once.
  still_ = value_ == goal_;
  dirty_ = true;
  const auto index = static_cast<std::size_t>(settings.model);
  if (index < filter_model_count) {
    target_model_ = settings.model;
  }
  if (!primed_) {
    // A fresh voice starts on its model; nothing to fade from.
    primed_ = true;
    if (slots_[active_].model != target_model_) {
      slots_[active_].model = target_model_;
      clear(active_);
    }
    fade_ = 0;
    return;
  }
  // A change during a fade waits for it to finish (process() starts it).
  if (fade_ == 0 && target_model_ != slots_[active_].model) {
    start_fade();
  }
  // The banked models derive exactly at the ramp's ends and blend their
  // coefficients between (see blends()). From the coefficients in use, so
  // the blend continues whatever the last ramp left.
  const auto small = [&](std::size_t i) { return std::fabs(goal_[i] - value_[i]) <= blend_step; };
  const auto model = slots_[active_].model;
  blend_ = !exact_ && ramp_ > 0 && !still_ && fade_ == 0 && blends(model) && small(0) && small(1) &&
           small(3) &&
           Impl::vowel_pair(model, params(value_)) == Impl::vowel_pair(model, params(goal_));
  if (blend_) {
    from_ = slots_[active_].c;
    Impl::derive(slots_[active_].model, params(goal_), sample_rate_, to_);
    span_ = ramp_;
  }
}

bool VoiceFilter::blends(FilterModel model) noexcept {
  return model == FilterModel::formant || model == FilterModel::vowel ||
         model == FilterModel::phaser;
}

VoiceFilter::Params VoiceFilter::params(const std::array<double, 4>& value) noexcept {
  return {std::clamp(std::exp2(value[0]), min_cutoff_hz, max_cutoff_hz),
          std::clamp(value[1], 0.0, 1.0), std::clamp(value[2], 0.0, max_drive_db),
          std::clamp(value[3], 0.0, 1.0)};
}

void VoiceFilter::start_fade() noexcept {
  active_ ^= 1U;
  slots_[active_].model = target_model_;
  clear(active_);
  fade_ = fade_length_;
  blend_ = false;
  refresh();
}

void VoiceFilter::refresh() noexcept {
  if (blend_) {
    // Linear in the coefficients between the ramp's exact ends; drive is
    // cheap and follows its ramp as before.
    auto& c = slots_[active_].c;
    if (ramp_ == 0) {
      c = to_;
    } else {
      const double t = static_cast<double>(span_ - ramp_) / static_cast<double>(span_);
      const auto mix = [t](double from, double to) { return from + t * (to - from); };
      c.output_gain = mix(from_.output_gain, to_.output_gain);
      c.k = mix(from_.k, to_.k);
      for (std::size_t b = 0; b < c.band.size(); ++b) {
        c.band[b].g = mix(from_.band[b].g, to_.band[b].g);
        c.band[b].k = mix(from_.band[b].k, to_.band[b].k);
        c.band[b].gain = mix(from_.band[b].gain, to_.band[b].gain);
      }
    }
    refresh_drive(std::clamp(value_[2], 0.0, max_drive_db));
    return;
  }
  const Params p = params(value_);
  refresh_drive(p.drive_db);
  Impl::derive(slots_[active_].model, p, sample_rate_, slots_[active_].c);
  if (fade_ > 0) {
    auto& old = slots_[active_ ^ 1U];
    Impl::derive(old.model, p, sample_rate_, old.c);
  }
}

void VoiceFilter::refresh_drive(double drive_db) noexcept {
  // The curve is a function of the drive alone: unchanged, it stands.
  if (drive_db == drive_db_) {
    return;
  }
  drive_db_ = drive_db;
  const DriveCurve d = drive_curve(drive_db);
  drive_mix_ = d.mix;
  drive_gain_ = d.gain;
  drive_bias_ = d.bias;
  drive_bias_level_ = soft_clip(d.bias);
  drive_slope_ = soft_clip_slope(d.bias);
  drive_makeup_ = d.makeup;
}

double VoiceFilter::drive(std::size_t channel, double x) noexcept {
  if (drive_mix_ <= 0.0) {
    return x;
  }
  // A slightly biased saturator split into its odd part (slope-normalised,
  // so small signals see exactly gain * makeup) and its even part, whose
  // DC a 10 Hz high-pass removes.
  const double u = drive_gain_ * x;
  const double up = soft_clip(u + drive_bias_);
  const double down = soft_clip(drive_bias_ - u);
  const double odd = 0.5 * (up - down) / drive_slope_;
  const double even = 0.5 * (up + down) - drive_bias_level_;
  const double high = even - dc_in_[channel] + dc_pole_ * dc_out_[channel];
  dc_in_[channel] = even;
  dc_out_[channel] = high;
  return x + drive_mix_ * (drive_makeup_ * (odd + high) - x);
}

void VoiceFilter::process(float& left, float& right) noexcept {
  // A ramp re-derives the coefficients every interval_ samples and at its
  // end. A still ramp (zero steps) keeps that timing, so a later ramp
  // refreshes on the same samples, but derives only once: its coefficients
  // cannot change.
  const bool ramping = ramp_ > 0;
  if (ramping) {
    --ramp_;
    if (ramp_ == 0) {
      value_ = goal_;
    } else if (!still_) {
      for (std::size_t i = 0; i < value_.size(); ++i) {
        value_[i] += step_[i];
      }
    }
    if (!still_) {
      dirty_ = true;
    }
  }
  if (dirty_ || ramping) {
    ++tick_;
    if (tick_ >= interval_ || ramp_ == 0) {
      tick_ = 0;
      if (dirty_) {
        refresh();
      }
      dirty_ = ramp_ > 0 && !still_;
    }
  }

  const bool fading = fade_ > 0;
  double old_weight = 0.0;
  if (fading) {
    --fade_;
    old_weight = static_cast<double>(fade_) / static_cast<double>(fade_length_);
  }
  const std::size_t mask = delay_size_ > 0 ? delay_size_ - 1U : 0U;
  const auto line = [&](std::size_t slot, std::size_t channel) -> float* {
    if (delay_size_ == 0 || !uses_delay(slots_[slot].model)) {
      return nullptr;
    }
    return delay_.data() + (channels * slot + channel) * delay_size_;
  };
  std::array<double, 2> io{left, right};
  bool failed = false;
  const auto model = slots_[active_].model;
  if (!fading && (model == FilterModel::ladder24 || model == FilterModel::ladder12 ||
                  model == FilterModel::mg_low || model == FilterModel::ota)) {
    // Both channels through one ladder (channels share no state, so the
    // order of their steps does not matter).
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const double x = io[channel];
      io[channel] =
          drive(channel, std::isfinite(x) ? std::clamp(x, -input_limit, input_limit) : 0.0);
    }
    Impl::ladder_pair(model, slots_[active_], io[0], io[1]);
    for (auto& y : io) {
      if (!std::isfinite(y)) {
        y = 0.0;
        failed = true;
      }
    }
  } else if (!fading && (model == FilterModel::formant || model == FilterModel::vowel)) {
    // The banked models' bands, both channels through one pass (channels
    // share no state).
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const double x = io[channel];
      io[channel] =
          drive(channel, std::isfinite(x) ? std::clamp(x, -input_limit, input_limit) : 0.0);
    }
    Impl::character_pair(slots_[active_], io[0], io[1]);
    for (auto& y : io) {
      if (!std::isfinite(y)) {
        y = 0.0;
        failed = true;
      }
    }
  } else {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      double x = io[channel];
      x = std::isfinite(x) ? std::clamp(x, -input_limit, input_limit) : 0.0;
      x = drive(channel, x);
      double y = Impl::run(slots_[active_], channel, x, line(active_, channel), mask);
      if (fading) {
        const std::size_t old = active_ ^ 1U;
        const double before = Impl::run(slots_[old], channel, x, line(old, channel), mask);
        y += old_weight * (before - y);
      }
      if (!std::isfinite(y)) {
        y = 0.0;
        failed = true;
      }
      io[channel] = y;
    }
  }
  ++slots_[active_].write;
  if (fading) {
    ++slots_[active_ ^ 1U].write;
  }
  if (failed) {
    // Unreachable for finite input by design; a guard so a fault can never
    // latch a voice into noise.
    clear(0);
    clear(1);
    fade_ = 0;
  }
  if (fading && fade_ == 0 && target_model_ != slots_[active_].model) {
    start_fade();
  }
  left = static_cast<float>(io[0]);
  right = static_cast<float>(io[1]);
}

} // namespace oscillate::native_dsp::synth::nova

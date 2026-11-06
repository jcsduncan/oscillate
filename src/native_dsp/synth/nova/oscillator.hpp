#pragma once

#include "modulation.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_dsp/synth/nova/tables.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <variant>

// Nova's main oscillators (spec 08 §3, NOVA-OSC-01, -02, -07..-09): Classic
// and Wavetable, each with up to 16 unison voices, two warps and
// cross-oscillator modulation. Sample, Multisample, Granular and Spectral
// arrive in phase 5 and render silence until then. Everything after
// construction is callback safe and allocation-free.
namespace oscillate::native_dsp::synth::nova {

inline constexpr std::size_t max_unison = 16;
// The most samples one render() call takes.
inline constexpr std::size_t max_render_samples = 256;

// The previous internal sample of every source, for cross modulation (one
// sample late, so any oscillator may modulate any other).
struct CrossInputs {
  std::array<float, 5> value{}; // A, B, C, Sub, Noise.
};

// One control tick's resolved settings for an oscillator (modulation
// already applied).
struct OscSettings {
  bool on = false;
  OscType type = OscType::classic;
  ClassicShape shape = ClassicShape::saw;
  FrameInterpolation interpolation = FrameInterpolation::crossfade;
  double level = 0.75, pan = 0.0, position = 0.0, pw = 0.5, sync = 1.0, drift = 0.15;
  std::array<WarpMode, warp_slots> warp_mode{WarpMode::off, WarpMode::off};
  std::array<double, warp_slots> warp_amount{};
  std::array<WarpSource, warp_slots> warp_source{WarpSource::osc_b, WarpSource::osc_b};
  std::uint32_t voices = 1;
  double detune_cents = 18.0;
  UnisonCurve curve = UnisonCurve::super;
  double blend = 0.6, width = 0.8;
  UnisonStack stack = UnisonStack::off;
  double position_spread = 0.0, warp_spread = 0.0;
  double semitones = 0.0; // Octave, semitone, fine and pitch summed.
  double global_drift = 0.1;
};

// Analogue's per-unison-voice offsets for one oscillator (ADR-0073), filled
// by the voice each tick; the oscillator is passed none while Analogue is
// off, so off renders exactly as before.
struct UnisonAnalogue {
  std::array<double, max_unison> cents{}; // Tolerance and drift.
  std::array<double, max_unison> gain{};  // Linear level, 1 unchanged.
  std::array<double, max_unison> pan{};   // Added to the voice's pan.
  std::array<double, max_unison> shape{}; // Added to position and warp amounts.
  // At a note's start: each voice's start-phase offset in cycles, or, when
  // free-running, its whole start phase.
  std::array<double, max_unison> phase{};
  bool free_running = false;
  UnisonAnalogue() noexcept { gain.fill(1.0); }
};

// The DC blockers' corner: an oscillator's own, and the voice's after its
// filters.
inline constexpr double dc_block_hz = 5.0;

// A Classic shape's band-limited oscillator, its kernel matched to the
// voice's oversampling so the shape stops at the output's Nyquist at any
// internal rate. Every kernel's tables are built on construction (control).
class ClassicBlep {
public:
  ClassicBlep() noexcept {
    (void)BasicBlepTables<clean_blep_2x>::instance();
    (void)BasicBlepTables<clean_blep_4x>::instance();
  }
  // At a note's start, before reset().
  void select(std::uint32_t oversample) noexcept {
    const std::size_t index = oversample >= 4 ? 2 : oversample >= 2 ? 1 : 0;
    if (index == osc_.index())
      return;
    if (index == 2)
      osc_.emplace<2>();
    else if (index == 1)
      osc_.emplace<1>();
    else
      osc_.emplace<0>();
  }
  void reset(double phase, Waveform waveform, double pulse_width = 0.5) noexcept {
    std::visit([&](auto& o) { o.reset(phase, waveform, pulse_width); }, osc_);
  }
  void set_waveform(Waveform waveform, double pulse_width) noexcept {
    std::visit([&](auto& o) { o.set_waveform(waveform, pulse_width); }, osc_);
  }
  float next(double increment, double pulse_width, SyncPoint sync, SyncPoint* wrap) noexcept {
    return std::visit([&](auto& o) { return o.next(increment, pulse_width, sync, wrap); }, osc_);
  }
  [[nodiscard]] double phase() const noexcept {
    return std::visit([](const auto& o) { return o.phase(); }, osc_);
  }

private:
  std::variant<BasicBlepOscillator<clean_blep>, BasicBlepOscillator<clean_blep_2x>,
               BasicBlepOscillator<clean_blep_4x>>
      osc_;
};

class Oscillator {
public:
  // At a note's start: the internal rate and the voice's oversampling.
  void prepare(double sample_rate, std::uint32_t oversample) noexcept {
    rate_ = sample_rate;
    dc_pole_ = std::exp(-2.0 * std::numbers::pi * dc_block_hz / sample_rate);
    for (auto& blep : bleps_)
      blep.select(oversample);
  }
  // A new note: unison phases from the start phase plus random_phase, drift
  // seeded from `seed`. Ramps start at the given settings. With `analogue`,
  // each unison voice's phase takes its offset (or its free-running phase).
  void start(const OscSettings& settings, double note_hz, const Wavetable* table,
             double phase_cycles, double random_phase, std::uint64_t seed,
             const UnisonAnalogue* analogue = nullptr) noexcept;
  // At a control tick: targets reached linearly over `frames` samples.
  void set(const OscSettings& settings, double note_hz, const Wavetable* table,
           const std::array<const CurveShape*, warp_slots>& remap, std::uint32_t frames,
           double tick_seconds, const UnisonAnalogue* analogue = nullptr) noexcept;
  // One sample: adds to left/right and returns the centred mono signal for
  // cross modulation.
  float process(const CrossInputs& cross, double& left, double& right) noexcept;
  // Switched off, its level faded to zero and its DC blocker settled: it
  // renders silence (and, read by no warp, need not run).
  [[nodiscard]] bool dormant() const noexcept {
    return !settings_.on && level_ramp_.value == 0.0 && level_ramp_.step == 0.0 &&
           dc_in_ == std::array<double, 2>{} && dc_out_ == std::array<double, 2>{};
  }
  // True when a warp reads another source (FM, PM, AM or RM), so the voice
  // must run its oscillators sample by sample, each a sample behind the rest.
  [[nodiscard]] bool reads_cross() const noexcept;
  // `count` samples (at most max_render_samples) of an oscillator that reads
  // no cross input: the arithmetic of `count` process() calls in the same
  // order, so the output is identical, but each unison voice runs through
  // the block on its own. Writes each sample's left, right and mono.
  void render(std::size_t count, double* left, double* right, float* mono) noexcept;
  // 08a §6: unison voices sounding, and one published cycle (128 points)
  // at the current settings.
  [[nodiscard]] std::uint32_t sounding() const noexcept;
  void snapshot(std::span<float, 128> out) const noexcept;
  [[nodiscard]] float peak() const noexcept { return peak_; }
  void decay_peak() noexcept { peak_ *= 0.5F; }

private:
  struct Ramp {
    double value = 0.0;
    double step = 0.0;
    void to(double target, std::uint32_t frames) noexcept {
      step = frames > 0 ? (target - value) / frames : 0.0;
      if (frames == 0)
        value = target;
    }
    double next() noexcept { return value += step; }
  };
  // A unison voice's motion. Its Classic oscillator (with its kilobyte
  // ring) lives apart in bleps_, so render() can copy a voice into
  // registers cheaply.
  struct Voice {
    double phase = 0.0;
    double master = 0.0; // Hard-sync master phase (Classic).
    Ramp increment, gain_left, gain_right, position, warp0, warp1;
    double presence = 0.0; // 0..1, fades a voice in or out over 5 ms.
    double drift = 0.0, drift_target = 0.0, drift_rate = 0.0;
    std::uint64_t seed = 0;
  };

  // One unison voice's next sample: advances its ramps and phase, and
  // returns false when it is silent. `fade` is level_fade_ at that sample.
  [[nodiscard]] bool step(Voice& voice, ClassicBlep& blep, double cross0, double cross1,
                          std::uint32_t fade, double& value, double& gain_left,
                          double& gain_right) const noexcept;
  [[nodiscard]] double read_table(double phase, double position, std::uint32_t fade) const noexcept;
  // render()'s common case, a Wavetable with both warps off and no level
  // fade: whether it applies to this chunk, and two unison voices through
  // it at once (see oscillator.cpp).
  [[nodiscard]] bool pairs_apply(std::size_t count) const noexcept;
  void render_pair(Voice& a, Voice& b, std::size_t count, double* sum_left, double* sum_right,
                   double* sum_mono) const noexcept;
  [[nodiscard]] double warp_phase(double phase, WarpMode mode, double amount, double cross,
                                  const CurveShape* remap) const noexcept;
  [[nodiscard]] static double warp_value(double value, WarpMode mode, double amount,
                                         double cross) noexcept;
  // A 5 Hz DC blocker on the output: phase warps and FM move a cycle's
  // average, which would otherwise reach the filters and the mix.
  double dc_block(std::size_t channel, double x) noexcept {
    const double y = x - dc_in_[channel] + dc_pole_ * dc_out_[channel];
    dc_in_[channel] = x;
    dc_out_[channel] = y;
    return y;
  }

  double rate_ = 96'000.0;
  OscSettings settings_{};
  const Wavetable* table_ = nullptr;
  std::array<const CurveShape*, warp_slots> remap_{};
  std::size_t level_ = 0;
  std::size_t previous_level_ = 0;
  std::uint32_t level_fade_ = 0;
  std::uint32_t level_fade_length_ = 1;
  std::array<Voice, max_unison> voices_{};
  std::array<ClassicBlep, max_unison> bleps_{};
  // Each unison slot's detune-curve offset for offsets_count_ voices on
  // offsets_curve_ (0 voices: not yet computed).
  std::array<double, max_unison> offsets_{};
  std::uint32_t offsets_count_ = 0;
  UnisonCurve offsets_curve_ = UnisonCurve::super;
  Ramp level_ramp_;
  float peak_ = 0.0F;
  double last_mono_ = 0.0;
  double dc_pole_ = 0.9997; // dc_block's pole.
  std::array<double, 2> dc_in_{}, dc_out_{};
};

// The sub oscillator (08a §2.3): a plain band-limited shape.
class SubOscillator {
public:
  void prepare(double sample_rate, std::uint32_t oversample) noexcept {
    rate_ = sample_rate;
    blep_.select(oversample);
  }
  void start(ClassicShape shape) noexcept;
  void set(double hz, double level, double pan, ClassicShape shape, std::uint32_t frames) noexcept;
  float process(double& left, double& right) noexcept;
  // Its level is zero and stays so until the next set(): it adds nothing.
  [[nodiscard]] bool silent() const noexcept {
    return gain_left_ == 0.0 && gain_left_step_ == 0.0 && gain_right_ == 0.0 &&
           gain_right_step_ == 0.0;
  }
  [[nodiscard]] float peak() const noexcept { return peak_; }
  void decay_peak() noexcept { peak_ *= 0.5F; }

private:
  double rate_ = 96'000.0;
  ClassicBlep blep_;
  double increment_ = 0.0, increment_step_ = 0.0;
  double gain_left_ = 0.0, gain_left_step_ = 0.0, gain_right_ = 0.0, gain_right_step_ = 0.0;
  float peak_ = 0.0F;
};

// Noise AM (08a §2.12c): the noise layer's gain for one sample of the
// followed oscillator `x` (its signal before its level knob, about -1..1).
// The signal is mapped to 0..1 and raised to `exponent` =
// 2^(curve × noise_am_curve_octaves): curve > 0 narrows the gain to the
// oscillator's peaks (gated, buzzier), curve < 0 holds it up through most of
// the cycle (smoother), 0 follows the wave linearly. Amount blends from 1
// (unchanged) to the full shape.
inline constexpr double noise_am_curve_octaves = 2.0;
[[nodiscard]] inline double noise_am_gain(double x, double amount, double exponent) noexcept {
  const double unipolar = std::clamp(0.5 * (x + 1.0), 0.0, 1.0);
  return 1.0 - amount + amount * std::pow(unipolar, exponent);
}

// The noise tilt's split frequency.
inline constexpr double noise_tilt_hz = 1'000.0;

// The noise oscillator (08a §2.3): a looped or one-shot noise sample read
// at a pitch, tilted dark to bright.
class NoiseOscillator {
public:
  void prepare(double sample_rate) noexcept {
    rate_ = sample_rate;
    tilt_ = native_dsp::dsp::one_pole_coefficient(noise_tilt_hz, rate_);
  }
  void start(const NoiseSample* sample, std::uint64_t seed) noexcept;
  void set(const NoiseSample* sample, double speed, double level, double pan, double tone,
           bool one_shot, std::uint32_t frames) noexcept;
  // One sample, its output scaled by `am` (noise AM; 1 leaves it exact).
  // Returns the unscaled noise for cross modulation.
  float process(double& left, double& right, double am = 1.0) noexcept;
  // Its level is zero and stays so until the next set(): it adds nothing.
  [[nodiscard]] bool silent() const noexcept {
    return gain_left_ == 0.0 && gain_left_step_ == 0.0 && gain_right_ == 0.0 &&
           gain_right_step_ == 0.0;
  }
  [[nodiscard]] float peak() const noexcept { return peak_; }
  void decay_peak() noexcept { peak_ *= 0.5F; }

private:
  double rate_ = 96'000.0;
  const NoiseSample* sample_ = nullptr;
  double position_ = 0.0, speed_ = 1.0;
  bool one_shot_ = false;
  double gain_left_ = 0.0, gain_left_step_ = 0.0, gain_right_ = 0.0, gain_right_step_ = 0.0;
  double tone_ = 0.0;
  // The tilt's one-pole coefficient: a 1 kHz split at the internal rate.
  double tilt_ = 0.0;
  StereoSample low_{};
  float peak_ = 0.0F;
};

} // namespace oscillate::native_dsp::synth::nova

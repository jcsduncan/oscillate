#include "oscillate/native_dsp/synth/torque.hpp"

#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/bass_dsp.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "torque_presets.hpp"
#include "torque_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::synth::torque {
namespace {

using torque_table::index_of;

namespace p {
constexpr std::size_t model = index_of("torque.model");
constexpr std::size_t tune = index_of("torque.tune");
constexpr std::size_t fine = index_of("torque.fine");
constexpr std::size_t cutoff = index_of("torque.cutoff");
constexpr std::size_t resonance = index_of("torque.resonance");
constexpr std::size_t envmod = index_of("torque.envmod");
constexpr std::size_t decay = index_of("torque.decay");
constexpr std::size_t env_attack = index_of("torque.env.attack");
constexpr std::size_t env_sustain = index_of("torque.env.sustain");
constexpr std::size_t keytrack = index_of("torque.keytrack");
constexpr std::size_t accent = index_of("torque.accent");
constexpr std::size_t accent_threshold = index_of("torque.accent_threshold");
constexpr std::size_t drive = index_of("torque.drive");
constexpr std::size_t slide = index_of("torque.slide");
constexpr std::size_t bend_range = index_of("torque.bend");
constexpr std::size_t bend = index_of("torque.perf.bend");
constexpr std::size_t wheel = index_of("torque.perf.wheel");
constexpr std::size_t amp_attack = index_of("torque.amp.attack");
constexpr std::size_t amp_decay = index_of("torque.amp.decay");
constexpr std::size_t amp_sustain = index_of("torque.amp.sustain");
constexpr std::size_t amp_release = index_of("torque.amp.release");
constexpr std::size_t acid_wave = index_of("torque.acid.wave");
constexpr std::size_t acid_tilt = index_of("torque.acid.tilt");
constexpr std::size_t moog_wave1 = index_of("torque.moog.wave1");
constexpr std::size_t moog_level2 = index_of("torque.moog.level2");
constexpr std::size_t moog_level3 = index_of("torque.moog.level3");
constexpr std::size_t moog_interval2 = index_of("torque.moog.interval2");
constexpr std::size_t moog_interval3 = index_of("torque.moog.interval3");
constexpr std::size_t moog_detune = index_of("torque.moog.detune");
constexpr std::size_t moog_noise = index_of("torque.moog.noise");
constexpr std::size_t reese_voices = index_of("torque.reese.voices");
constexpr std::size_t reese_detune = index_of("torque.reese.detune");
constexpr std::size_t reese_spread = index_of("torque.reese.spread");
constexpr std::size_t reese_sub = index_of("torque.reese.sub");
constexpr std::size_t reese_filter = index_of("torque.reese.filter");
constexpr std::size_t reese_comb = index_of("torque.reese.comb");
constexpr std::size_t sub_drop = index_of("torque.sub.drop");
constexpr std::size_t sub_drop_time = index_of("torque.sub.drop_time");
constexpr std::size_t sub_layer = index_of("torque.sub.layer");
constexpr std::size_t sub_layer_level = index_of("torque.sub.layer_level");
constexpr std::size_t lfo_shape = index_of("torque.lfo.shape");
constexpr std::size_t lfo_rate = index_of("torque.lfo.rate");
constexpr std::size_t lfo_sync = index_of("torque.lfo.sync");
constexpr std::size_t lfo_division = index_of("torque.lfo.division");
constexpr std::size_t lfo_cutoff = index_of("torque.lfo.cutoff");
constexpr std::size_t lfo_pitch = index_of("torque.lfo.pitch");
constexpr std::size_t lfo_movement = index_of("torque.lfo.movement");
constexpr std::size_t dist_mode = index_of("torque.fx.dist_mode");
constexpr std::size_t dist_drive = index_of("torque.fx.dist_drive");
constexpr std::size_t dist_tone = index_of("torque.fx.dist_tone");
constexpr std::size_t dist_mix = index_of("torque.fx.dist_mix");
constexpr std::size_t chorus = index_of("torque.fx.chorus");
constexpr std::size_t delay = index_of("torque.fx.delay");
constexpr std::size_t delay_time = index_of("torque.fx.delay_time");
constexpr std::size_t delay_feedback = index_of("torque.fx.delay_feedback");
constexpr std::size_t delay_sync = index_of("torque.fx.delay_sync");
constexpr std::size_t delay_division = index_of("torque.fx.delay_division");
constexpr std::size_t out_level = index_of("torque.out.level");
constexpr std::size_t seq_on = index_of("torque.seq.on");
constexpr std::size_t seq_length = index_of("torque.seq.length");
constexpr std::size_t seq_rate = index_of("torque.seq.rate");
constexpr std::size_t seq_swing = index_of("torque.seq.swing");
constexpr std::size_t seq_gate = index_of("torque.seq.gate");
static_assert(index_of("torque.seq.s16.slide") == torque_table::parameters.size() - 1,
              "the table has no zero-filled entries");
} // namespace p

enum class Model : std::uint8_t { acid, moog, reese, sub };

constexpr double voice_gain = 0.5;
constexpr double filter_env_semitones = 48.0;
constexpr double sweep_semitones = 30.0;
constexpr std::size_t max_frames = control_interval_frames;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] double hz(double semitones) noexcept {
  return 440.0 * std::exp2((semitones - 69.0) / 12.0);
}

using Ramp = dsp::Ramp;

// One channel of the oversampled path: filter, drive, VCA and distortion.
struct Channel {
  Upsampler2x up{};
  Downsampler2x down{};
  DiodeLadder diode{};
  ZdfLadder ladder{};
  MultimodeFilter multi{};
  CombFilter comb{};
  BassDistortion distortion{};
  void reset() noexcept {
    up.reset();
    down.reset();
    diode.reset();
    ladder.reset();
    multi.reset();
    comb.reset();
    distortion.reset();
  }
};

// Everything one model renders with: its oscillators, their ramps and its
// oversampled filter chain. The voice keeps a second, fading state while a
// model or Reese voice-count change crossfades on a sounding note
// (ADR-0038); envelopes, cutoff and level stay shared.
struct ModelState {
  Model model = Model::acid;
  std::uint32_t reese_count = 5;
  std::array<BlepOscillator, max_reese_voices> oscillators{};
  BlepOscillator sub_oscillator{};
  std::array<Ramp, max_reese_voices + 1> increment{};
  std::array<Ramp, 4> level{};
  std::array<double, max_reese_voices> left{};
  std::array<double, max_reese_voices> right{};
  Ramp tilt{};
  SquareTilt square_tilt{};
  double sub_phase = 0.0;
  double layer_phase = 0.0;
  double drop_excess = 0.0;
  double drop_decay = 0.999;
  std::array<Channel, 2> channels{};
};

class TorqueKernel final : public SynthKernel {
public:
  explicit TorqueKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames) {
    amp_.prepare(rate_);
    filter_env_.prepare(rate_);
    sweep_.prepare(control_rate_);
    lfo_.prepare(control_rate_);
    // Both states are prepared alike, so copying one into the other on a
    // switch reuses the comb lines' storage and never allocates.
    for (auto* state : {&state_, &fading_}) {
      for (auto& channel : state->channels) {
        channel.comb.prepare(rate_);
        channel.distortion.prepare(2.0 * rate_);
      }
    }
    fade_.prepare(rate_);
    chorus_.prepare(rate_);
    delay_.prepare(rate_);
    dc_[0].prepare(rate_);
    dc_[1].prepare(rate_);
    reset_voice();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    values_ = values;
    frames_ = frames_to_next_tick;
    beats_per_frame_ = transport.beats_per_frame > 0.0 ? transport.beats_per_frame : 2.0 / rate_;
    const auto& v = values;
    const auto division = static_cast<std::size_t>(v[p::lfo_division]);
    lfo_.set_shape(lfo_shape());
    if (v[p::lfo_sync] != 0.0 && transport.beats_per_frame > 0.0) {
      lfo_value_ = lfo_.next_at(sync_cycles(division, transport));
    } else {
      const double rate =
          v[p::lfo_sync] != 0.0 ? sync_hz(division, transport, rate_) : v[p::lfo_rate];
      lfo_value_ = lfo_.next(rate / control_rate_);
    }
    read_pattern();
    // A note triggered by the switch has had this tick's control update.
    const bool triggered = reconcile_sequencer(v[p::seq_on] != 0.0);
    const double steps_per_beat =
        torque_table::seq_steps_per_beat[static_cast<std::size_t>(v[p::seq_rate])];
    sequencer_.set_timing(1.0 / (beats_per_frame_ * steps_per_beat), v[p::seq_swing],
                          v[p::seq_gate]);
    if (active_ && !triggered) {
      control_voice(false);
    }
    // Effects follow the panel smoothly.
    const auto ramp = frames_;
    for (auto* state : {&state_, &fading_}) {
      for (auto& channel : state->channels) {
        channel.distortion.set(static_cast<BassDistortionMode>(as_int(v[p::dist_mode])),
                               v[p::dist_drive], v[p::dist_tone], v[p::dist_mix]);
      }
    }
    chorus_.set(static_cast<ChorusMode>(as_int(v[p::chorus])), 1.0);
    const double delay_seconds =
        v[p::delay_sync] != 0.0
            ? sync_seconds(static_cast<std::size_t>(v[p::delay_division]), transport, rate_)
            : v[p::delay_time];
    delay_.set(delay_seconds, v[p::delay_feedback], v[p::delay], true, ramp);
    const double level = v[p::out_level] <= -59.9 ? 0.0 : std::pow(10.0, v[p::out_level] / 20.0);
    out_gain_.to(level, ramp, !primed_);
    primed_ = true;
  }

  // Brings the voice in line with the sequencer switch while notes are held:
  // switched on, the pattern starts rooted on the newest held note; switched
  // off, the pattern stops and the newest held note plays on its own.
  // True when it triggered a note (which ran its own control update).
  bool reconcile_sequencer(bool on) noexcept {
    if (on && !sequencer_.running() && stack_size_ > 0) {
      root_ = stack_[stack_size_ - 1].key;
      release();
      sequencer_.start();
    } else if (!on && sequencer_.running()) {
      sequencer_.stop();
      if (stack_size_ > 0) {
        const auto& newest = stack_[stack_size_ - 1];
        trigger(newest.key, newest.velocity >= values_[p::accent_threshold]);
        sounding_ = newest;
        return true;
      }
      release();
    }
    return false;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty()) {
      return;
    }
    if (stack_size_ < stack_.size()) {
      stack_[stack_size_++] = event;
    }
    if (values_[p::seq_on] != 0.0) {
      // The sequencer runs while a note is held, rooted on the newest note.
      root_ = event.key;
      if (!sequencer_.running()) {
        sequencer_.start();
      }
      return;
    }
    const bool accented = event.velocity >= values_[p::accent_threshold];
    if (gate_ && active_) {
      slide_to(event.key, accented); // Overlap: slide, no retrigger.
    } else {
      trigger(event.key, accented);
    }
    sounding_ = event;
  }

  void note_off(const NoteEvent& event) noexcept override {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < stack_size_; ++i) {
      if (!(stack_[i].clip == event.clip && stack_[i].note == event.note)) {
        stack_[kept++] = stack_[i];
      }
    }
    stack_size_ = kept;
    if (sequencer_.running()) {
      if (stack_size_ == 0) {
        sequencer_.stop();
        release();
      } else {
        root_ = stack_[stack_size_ - 1].key;
      }
      return;
    }
    if (!(sounding_.clip == event.clip && sounding_.note == event.note)) {
      return;
    }
    if (stack_size_ > 0) {
      // Legato back to the newest held note.
      const auto& previous = stack_[stack_size_ - 1];
      slide_to(previous.key, previous.velocity >= values_[p::accent_threshold]);
      sounding_ = previous;
      return;
    }
    release();
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const auto frames = static_cast<std::uint32_t>(output.size());
    std::uint32_t done = 0;
    const auto pattern = std::span(pattern_).first(pattern_length_);
    while (auto event = sequencer_.peek(pattern, frames - done)) {
      render_span(output.subspan(done, event->offset));
      sequencer_.advance(event->offset);
      done += event->offset;
      sequencer_.consume(pattern);
      apply(*event);
    }
    render_span(output.subspan(done));
    sequencer_.advance(frames - done);
    // Retire only on the control grid, so the moment never depends on how
    // the host split its blocks.
    if (frames >= frames_ && active_ && !amp_.active()) {
      active_ = false;
    }
    frames_ -= std::min(frames, frames_);
  }

  void reset() noexcept override {
    if (active_) {
      capture_declick();
    }
    reset_voice();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto step = sequencer_.running() && sequencer_.current_step() >= 0
                          ? sequencer_.current_step() % static_cast<std::int64_t>(pattern_length_)
                          : -1;
    out[telemetry::running] = sequencer_.running() ? 1.0F : 0.0F;
    out[telemetry::step] = static_cast<float>(step);
    out[telemetry::key] = active_ ? static_cast<float>(pitch_) : -1.0F;
    out[telemetry::sweep] = static_cast<float>(sweep_.value());
    out[telemetry::cutoff] = static_cast<float>(last_cutoff_);
    out[telemetry::gate] = gate_ ? 1.0F : 0.0F;
    return telemetry::size;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    if (active_) {
      status.active_voices = 1;
    }
    return status;
  }

private:
  [[nodiscard]] LfoShape lfo_shape() const noexcept {
    switch (as_int(values_[p::lfo_shape])) {
    case 1:
      return LfoShape::triangle;
    case 2:
      return LfoShape::saw_down;
    case 3:
      return LfoShape::square;
    case 4:
      return LfoShape::sample_hold;
    default:
      return LfoShape::sine;
    }
  }

  void read_pattern() noexcept {
    const auto& v = values_;
    for (std::size_t s = 0; s < torque_table::step_count; ++s) {
      const std::size_t base = torque_table::first_step + s * torque_table::step_parameters;
      auto& step = pattern_[s];
      step.note = as_int(v[base]);
      step.octave = as_int(v[base + 1]);
      step.gate = static_cast<StepGate>(as_int(v[base + 2]));
      step.accent = v[base + 3] != 0.0;
      step.slide = v[base + 4] != 0.0;
    }
    pattern_length_ = static_cast<std::size_t>(v[p::seq_length]);
  }

  void reset_voice() noexcept {
    active_ = false;
    gate_ = false;
    stack_size_ = 0;
    sequencer_.stop();
    amp_.reset();
    filter_env_.reset();
    sweep_.reset();
    lfo_.reset(0.0, 0x70E0U);
    noise_.reset(0x5EEDU);
    for (auto* state : {&state_, &fading_}) {
      for (auto& channel : state->channels) {
        channel.reset();
      }
    }
    fade_.stop();
    amp_previous_ = 0.0;
  }

  void capture_declick() noexcept { declick_.capture(last_left_, last_right_); }

  void apply(const SequencerEvent& event) noexcept {
    const double key = static_cast<double>(root_) + event.semitones;
    switch (event.kind) {
    case SequencerEvent::Kind::note_on:
      trigger(key, event.accent);
      break;
    case SequencerEvent::Kind::glide:
      slide_to(key, event.accent);
      break;
    case SequencerEvent::Kind::note_off:
      release();
      break;
    }
  }

  // ---- Notes ----------------------------------------------------------------

  void set_envelopes() noexcept {
    const auto& v = values_;
    amp_.set({v[p::amp_attack], v[p::amp_decay], v[p::amp_sustain], v[p::amp_release], 0.6});
    filter_env_.set({v[p::env_attack], v[p::decay], v[p::env_sustain], v[p::amp_release], 0.8});
  }

  void trigger(double key, bool accented) noexcept {
    const auto& v = values_;
    if (!active_) {
      state_.model = static_cast<Model>(as_int(v[p::model]));
      state_.reese_count = static_cast<std::uint32_t>(v[p::reese_voices]);
      for (std::size_t i = 0; i < state_.oscillators.size(); ++i) {
        // Fixed, spread start phases: every note starts the same way.
        state_.oscillators[i].reset(0.137 * static_cast<double>(i), Waveform::saw);
      }
      state_.sub_oscillator.reset(0.0, Waveform::sine);
      state_.sub_phase = 0.0;
      state_.layer_phase = 0.0;
      for (auto& channel : state_.channels) {
        channel.reset();
      }
      fade_.stop();
      amp_.reset();
      filter_env_.reset();
      amp_previous_ = 0.0;
      active_ = true;
    }
    target_ = key;
    pitch_ = key;
    accented_ = accented;
    gate_ = true;
    set_envelopes();
    amp_.gate_on();
    filter_env_.gate_on();
    // The 808 drop restarts with each attack; it falls in Hz, like ResonantBody.
    const double base = note_hz();
    state_.drop_excess = base * (std::exp2(v[p::sub_drop] / 12.0) - 1.0);
    control_voice(true);
  }

  void slide_to(double key, bool accented) noexcept {
    if (!active_) {
      trigger(key, accented);
      return;
    }
    target_ = key;
    accented_ = accented;
    gate_ = true;
  }

  void release() noexcept {
    gate_ = false;
    amp_.gate_off();
    filter_env_.gate_off();
  }

  [[nodiscard]] double note_hz() const noexcept {
    const auto& v = values_;
    const double vibrato = lfo_value_ * v[p::lfo_pitch] * 2.0;
    return hz(pitch_ + v[p::tune] + v[p::fine] / 100.0 + v[p::bend] * v[p::bend_range] + vibrato);
  }

  // ---- Control rate ------------------------------------------------------------

  void control_voice(bool jump) noexcept {
    const auto& v = values_;
    const std::uint32_t frames = frames_;
    if (!jump && pitch_ != target_) {
      // Exponential slide: tau = slide / 3, so it is 95 % there by the slide time.
      const double tau = v[p::slide] / 3.0;
      pitch_ += (target_ - pitch_) * (1.0 - std::exp(-1.0 / (tau * control_rate_)));
      if (std::fabs(target_ - pitch_) < 1e-4) {
        pitch_ = target_;
      }
    }
    set_envelopes();
    const double f0 = note_hz();
    const double movement = lfo_value_ * v[p::lfo_movement];

    // A model or Reese voice-count change reaches the sounding note
    // (ADR-0038): the old state keeps rendering, warm, and crossfades out. A
    // change that arrives during a fade waits for it.
    const auto model = static_cast<Model>(as_int(v[p::model]));
    const auto reese = static_cast<std::uint32_t>(v[p::reese_voices]);
    if (!jump && !fade_.active() && (model != state_.model || reese != state_.reese_count)) {
      fading_ = state_;
      switch_model(model, reese);
      fade_.start();
    }
    control_model(state_, f0, movement, frames, jump || fresh_state_);
    fresh_state_ = false;
    if (fade_.active()) {
      control_model(fading_, f0, movement, frames, false);
    }

    // Filter: cutoff in semitones; the envelope and the accent sweep are
    // added per frame.
    const double lfo_cut = 24.0 * lfo_value_ * (v[p::lfo_cutoff] + 0.5 * v[p::wheel]);
    const double base =
        std::log2(v[p::cutoff]) + (v[p::keytrack] * (pitch_ - 60.0) + lfo_cut) / 12.0;
    cutoff_log2_.to(base, frames, jump);
    const double accent = accented_ ? v[p::accent] : 0.0;
    env_depth_.to(v[p::envmod] * (1.0 + 0.5 * accent) * filter_env_semitones / 12.0, frames, jump);
    const double sweep = sweep_.next(filter_env_.value(), accented_ && gate_);
    sweep_octaves_.to(v[p::accent] * sweep * sweep_semitones / 12.0, frames, jump);
    resonance_.to(v[p::resonance], frames, jump);
    drive_.to(1.0 + 5.0 * v[p::drive] * v[p::drive], frames, jump);
    gain_.to(voice_gain * (1.0 + accent), frames, jump);
  }

  // Takes the new model or voice count. Oscillators and filters the old
  // configuration did not use start clean; ones it shared carry on.
  void switch_model(Model model, std::uint32_t reese) noexcept {
    const auto used = [](Model m, std::uint32_t count) -> std::size_t {
      switch (m) {
      case Model::acid:
        return 1;
      case Model::moog:
        return 3;
      case Model::reese:
        return count;
      case Model::sub:
        return 0;
      }
      return 0;
    };
    const std::size_t had = used(state_.model, state_.reese_count);
    const std::size_t needs = used(model, reese);
    for (std::size_t i = had; i < needs; ++i) {
      state_.oscillators[i].reset(0.137 * static_cast<double>(i), Waveform::saw);
    }
    if (model == Model::reese && state_.model != Model::reese) {
      state_.sub_oscillator.reset(0.0, Waveform::sine);
    }
    if (model != state_.model) {
      // A different filter topology starts from rest; the fade hides it.
      for (auto& channel : state_.channels) {
        channel.diode.reset();
        channel.ladder.reset();
        channel.multi.reset();
        channel.comb.reset();
      }
    }
    state_.model = model;
    state_.reese_count = reese;
    fresh_state_ = true;
  }

  void control_model(ModelState& state, double f0, double movement, std::uint32_t frames,
                     bool jump) noexcept {
    const auto& v = values_;
    switch (state.model) {
    case Model::acid: {
      const bool square = v[p::acid_wave] != 0.0;
      state.oscillators[0].set_waveform(square ? Waveform::pulse : Waveform::saw, 0.5);
      state.increment[0].to(f0 / rate_, frames, jump);
      // The square's tops sag towards zero at a corner that tracks the note.
      const double corner = f0 * (0.02 + 1.2 * v[p::acid_tilt] * (1.0 + 0.5 * movement));
      state.tilt.to(square ? native_dsp::dsp::one_pole_coefficient(corner, rate_) : 0.0, frames,
                    jump);
      break;
    }
    case Model::moog: {
      const double detune = v[p::moog_detune] / 100.0;
      const std::array<double, 3> semis{0.0, v[p::moog_interval2] + detune,
                                        v[p::moog_interval3] - detune};
      const std::array<double, 3> levels{1.0, v[p::moog_level2], v[p::moog_level3]};
      const double norm = 1.0 / std::sqrt(levels[0] + levels[1] + levels[2]);
      for (std::size_t i = 0; i < 3; ++i) {
        state.oscillators[i].set_waveform(moog_wave(as_int(v[p::moog_wave1 + i])), 0.5);
        state.increment[i].to(f0 * std::exp2(semis[i] / 12.0) / rate_, frames, jump);
        state.level[i].to(levels[i] * norm, frames, jump);
      }
      state.level[3].to(0.5 * v[p::moog_noise], frames, jump);
      break;
    }
    case Model::reese: {
      const std::uint32_t count = state.reese_count;
      const double detune = v[p::reese_detune] * (1.0 + 0.6 * movement);
      const double norm = 1.0 / std::sqrt(static_cast<double>(count));
      for (std::uint32_t i = 0; i < count; ++i) {
        const double position = -1.0 + 2.0 * i / static_cast<double>(count - 1);
        state.oscillators[i].set_waveform(Waveform::saw, 0.5);
        state.increment[i].to(f0 * std::exp2(position * detune / 2.0 / 1'200.0) / rate_, frames,
                              jump);
        // Alternate sides so neighbouring detunes spread wide.
        const double pan = v[p::reese_spread] * position * (i % 2 == 0 ? 1.0 : -1.0);
        state.left[i] = (pan > 0.0 ? 1.0 - pan : 1.0) * norm;
        state.right[i] = (pan < 0.0 ? 1.0 + pan : 1.0) * norm;
      }
      state.increment[max_reese_voices].to(0.5 * f0 / rate_, frames, jump);
      state.level[0].to(0.8 * v[p::reese_sub], frames, jump);
      const double comb_hz = clamp(f0 * std::exp2(1.5 * movement), 20.0, 0.4 * rate_);
      for (auto& channel : state.channels) {
        channel.comb.set(comb_hz, -0.75 * v[p::reese_comb], frames);
      }
      state.level[1].to(v[p::reese_comb], frames, jump);
      break;
    }
    case Model::sub: {
      state.increment[0].to(f0 / rate_, frames, jump);
      const int layer = as_int(v[p::sub_layer]);
      state.increment[1].to((layer == 2 ? 2.0 : 1.5) * f0 / rate_, frames, jump);
      state.level[1].to(layer == 0 ? 0.0 : v[p::sub_layer_level], frames, jump);
      state.drop_decay = std::exp(-1.0 / std::max(1.0, v[p::sub_drop_time] * rate_));
      break;
    }
    }
  }

  [[nodiscard]] static Waveform moog_wave(int choice) noexcept {
    switch (choice) {
    case 1:
      return Waveform::triangle;
    case 2:
      return Waveform::pulse;
    default:
      return Waveform::saw;
    }
  }

  // ---- Audio rate ---------------------------------------------------------------

  void render_span(std::span<engine::StereoFrame> output) noexcept {
    const std::size_t frames = output.size();
    if (frames == 0) {
      return;
    }
    std::array<double, max_frames> left{};
    std::array<double, max_frames> right{};
    if (active_) {
      render_voice(left, right, frames);
    }
    for (std::size_t i = 0; i < frames; ++i) {
      StereoSample frame{left[i], right[i]};
      declick_.add(frame);
      double l = frame.left;
      double r = frame.right;
      const auto chorused = chorus_.process(l, r);
      const auto delayed = delay_.process(chorused.left, chorused.right);
      const double gain = out_gain_.next();
      // Output coupling, as on the hardware: short enveloped bass notes and
      // asymmetric drive leave sub-audio offsets that this removes; its
      // settling is part of the tail bound.
      output[i].left += static_cast<float>(safety(dc_[0].process(delayed.left) * gain));
      output[i].right += static_cast<float>(safety(dc_[1].process(delayed.right) * gain));
    }
  }

  void oscillate(ModelState& state, std::array<double, max_frames>& left,
                 std::array<double, max_frames>& right, std::size_t frames, bool& stereo) noexcept {
    switch (state.model) {
    case Model::acid:
      for (std::size_t i = 0; i < frames; ++i) {
        const double x = state.oscillators[0].next(state.increment[0].next(), 0.5, {}, nullptr);
        const double tilt = state.tilt.next();
        left[i] = 0.8 * (tilt > 0.0 ? state.square_tilt.process(x, tilt) : x);
      }
      break;
    case Model::moog:
      for (std::size_t i = 0; i < frames; ++i) {
        double x = 0.0;
        for (std::size_t o = 0; o < 3; ++o) {
          x += state.oscillators[o].next(state.increment[o].next(), 0.5, {}, nullptr) *
               state.level[o].next();
        }
        left[i] = x + noise_.white() * state.level[3].next();
      }
      break;
    case Model::reese: {
      stereo = true;
      for (std::size_t i = 0; i < frames; ++i) {
        double l = 0.0;
        double r = 0.0;
        for (std::uint32_t o = 0; o < state.reese_count; ++o) {
          const double x = state.oscillators[o].next(state.increment[o].next(), 0.5, {}, nullptr);
          l += x * state.left[o];
          r += x * state.right[o];
        }
        const double sub =
            state.sub_oscillator.next(state.increment[max_reese_voices].next(), 0.5, {}, nullptr) *
            state.level[0].next();
        left[i] = l + sub;
        right[i] = r + sub;
      }
      break;
    }
    case Model::sub:
      for (std::size_t i = 0; i < frames; ++i) {
        const double base = state.increment[0].next();
        state.sub_phase += base + state.drop_excess / rate_;
        state.sub_phase -= fast_floor(state.sub_phase);
        state.drop_excess *= state.drop_decay;
        state.layer_phase += state.increment[1].next();
        state.layer_phase -= fast_floor(state.layer_phase);
        left[i] = sin_cycle(state.sub_phase) + state.level[1].next() * sin_cycle(state.layer_phase);
      }
      break;
    }
  }

  void render_voice(std::array<double, max_frames>& left, std::array<double, max_frames>& right,
                    std::size_t frames) noexcept {
    // Per-frame filter coefficients: the envelope moves the cutoff at audio
    // rate, as on the hardware. Shared by both states during a fade.
    Shared shared;
    const double fast = 2.0 * rate_;
    for (std::size_t i = 0; i < frames; ++i) {
      const double env = filter_env_.next();
      const double octaves = cutoff_log2_.next() + env_depth_.next() * env + sweep_octaves_.next();
      shared.cutoff[i] = clamp(std::exp2(octaves), 20.0, native_dsp::dsp::nyquist_guard * fast);
      last_cutoff_ = shared.cutoff[i];
      shared.amp[i] = amp_.next() * gain_.next();
      shared.drive[i] = drive_.next();
      shared.resonance[i] = resonance_.next();
    }
    render_state(state_, shared, left, right, frames);
    if (fade_.active()) {
      std::array<double, max_frames> old_left{};
      std::array<double, max_frames> old_right{};
      render_state(fading_, shared, old_left, old_right, frames);
      for (std::size_t i = 0; i < frames; ++i) {
        const double weight = fade_.next();
        left[i] += (old_left[i] - left[i]) * weight;
        right[i] += (old_right[i] - right[i]) * weight;
      }
    }
    amp_previous_ = frames > 0 ? shared.amp[frames - 1] : amp_previous_;
    last_left_ = left[frames - 1];
    last_right_ = right[frames - 1];
  }

  struct Shared {
    std::array<double, max_frames> cutoff{};
    std::array<double, max_frames> amp{};
    std::array<double, max_frames> drive{};
    std::array<double, max_frames> resonance{};
  };

  // One state: oscillators, the Reese comb, then the oversampled filter,
  // VCA and distortion per channel.
  void render_state(ModelState& state, const Shared& shared, std::array<double, max_frames>& left,
                    std::array<double, max_frames>& right, std::size_t frames) noexcept {
    bool stereo = false;
    oscillate(state, left, right, frames, stereo);
    if (state.model == Model::reese) {
      for (std::size_t i = 0; i < frames; ++i) {
        const double comb = state.level[1].next();
        left[i] += comb * (state.channels[0].comb.process(left[i]) - left[i]);
        right[i] += comb * (state.channels[1].comb.process(right[i]) - right[i]);
      }
    }
    const std::size_t channels = stereo ? 2 : 1;
    for (std::size_t c = 0; c < channels; ++c) {
      auto& channel = state.channels[c];
      auto& data = c == 0 ? left : right;
      double previous = amp_previous_;
      for (std::size_t i = 0; i < frames; ++i) {
        double a = 0.0;
        double b = 0.0;
        const double drive = shared.drive[i];
        channel.up.process(data[i] * drive, a, b);
        filter_pair(channel, state.model, a, b, shared.cutoff[i], shared.resonance[i]);
        const double makeup = 1.0 / (1.0 + 0.35 * std::log(drive));
        a = channel.distortion.process(a * 0.5 * (previous + shared.amp[i]) * makeup);
        b = channel.distortion.process(b * shared.amp[i] * makeup);
        previous = shared.amp[i];
        data[i] = channel.down.process(a, b);
      }
    }
    if (!stereo) {
      right = left;
    }
  }

  // The voice drives each filter's saturating input; drive is applied before.
  void filter_pair(Channel& channel, Model model, double& a, double& b, double cutoff_hz,
                   double resonance) const noexcept {
    const double fast = 2.0 * rate_;
    switch (model) {
    case Model::acid: {
      const auto k = DiodeLadderCoefficients::make(cutoff_hz, fast, resonance);
      a = channel.diode.process(a, k);
      b = channel.diode.process(b, k);
      break;
    }
    case Model::moog: {
      const auto k =
          LadderCoefficients::make(prewarp(cutoff_hz / fast), resonance * max_ladder_feedback);
      a = channel.ladder.process(a, k);
      b = channel.ladder.process(b, k);
      break;
    }
    case Model::reese: {
      const auto mode =
          values_[p::reese_filter] != 0.0 ? FilterMode::bandpass_12 : FilterMode::lowpass_24;
      const auto k = make_filter_coefficients(mode, cutoff_hz / fast, resonance, 1.0);
      a = soft_clip(channel.multi.process(a, k));
      b = soft_clip(channel.multi.process(b, k));
      break;
    }
    case Model::sub: {
      const auto k =
          make_filter_coefficients(FilterMode::lowpass_12, cutoff_hz / fast, 0.5 * resonance, 1.0);
      a = channel.multi.process(soft_clip(a), k);
      b = channel.multi.process(soft_clip(b), k);
      break;
    }
    }
  }

  [[nodiscard]] static double safety(double x) noexcept {
    constexpr double knee = 0.9;
    const double magnitude = std::fabs(x);
    if (magnitude <= knee) {
      return x;
    }
    const double shaped = knee + (1.0 - knee) * soft_clip((magnitude - knee) / (1.0 - knee) * 3.0);
    return x < 0.0 ? -shaped : shaped;
  }

  double rate_;
  double control_rate_;
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  double beats_per_frame_ = 0.0;

  // Voice
  bool active_ = false;
  bool gate_ = false;
  bool accented_ = false;
  double pitch_ = 48.0;
  double target_ = 48.0;
  std::array<NoteEvent, 16> stack_{};
  std::size_t stack_size_ = 0;
  NoteEvent sounding_{};
  Adsr amp_{};
  Adsr filter_env_{};
  AccentSweep sweep_{};
  double amp_previous_ = 0.0;
  double last_cutoff_ = 0.0;
  ModelState state_{};
  ModelState fading_{}; // The previous model while fade_ runs.
  SwitchFade fade_{};
  bool fresh_state_ = false; // A new model's ramps jump once.
  NoiseSource noise_{};
  Ramp cutoff_log2_, env_depth_, sweep_octaves_, resonance_, drive_, gain_;
  double last_left_ = 0.0;
  double last_right_ = 0.0;

  // Modulation and sequencer
  Lfo lfo_{};
  double lfo_value_ = 0.0;
  StepSequencer sequencer_{};
  std::array<SequencerStep, torque_table::step_count> pattern_{};
  std::size_t pattern_length_ = 16;
  int root_ = 48;

  // Output
  Declick declick_;
  Chorus chorus_{};
  BbdDelay delay_{};
  Ramp out_gain_{};
  bool primed_ = false;
  std::array<DcBlocker, 2> dc_{};
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<TorqueKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const double delay_seconds =
      values[p::delay] > 0.0
          ? BbdDelay::tail_seconds(BbdDelay::max_seconds, values[p::delay_feedback])
          : 0.0;
  // The 5 Hz output coupling settles in about nine time constants (0.29 s).
  constexpr double coupling_seconds = 0.3;
  const double seconds = 1.15 * values[p::amp_release] + delay_seconds + Chorus::tail_seconds +
                         coupling_seconds + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate)) +
         BlepOscillator::latency_frames;
}

const EngineDescriptor descriptor{"torque", "Torque", 1, torque_table::parameters,
                                  torque_presets::presets};
const SynthEngine torque_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return torque_engine; }

} // namespace oscillate::native_dsp::synth::torque
